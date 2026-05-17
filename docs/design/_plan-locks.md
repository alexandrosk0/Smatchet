# Plan-locks — parallel-plan coordination scratchpad

Single source of truth for **which design plan is currently writing which files**. Every multi-phase or multi-PR plan in `docs/design/` claims its in-flight write set here so concurrent plans (and their delegated agents) can spot collisions before they happen.

This file replaces ad-hoc "I'll edit the same file by accident" failures with an explicit, lightweight handshake. Append-only entries; status transitions in-place.

## Protocol

Every plan that ships in more than one PR (or that hands off to delegated agents) appends an entry here **before** the first edit and updates it on every state transition.

**Entry shape:**

```
### <plan-slug> · <slice-id> · status: <claimed|in-flight|shipped|on-hold|abandoned>

- **Branch**: `claude/<branch-name>` (or `feat/<branch-name>` for autonomous-plan branches)
- **Owner agent**: `<agent-name>` (or `orchestrator` if direct)
- **Originating plan**: [`docs/design/<plan>.md`](./<plan>.md) § <section>
- **Claimed write set** (paths the slice will edit / create / delete):
  - `<path 1>`
  - `<path 2>`
  - ...
- **Read-only adjacency** (paths the slice reads but does not edit — list when high overlap risk):
  - `<path>`
- **Started**: `<YYYY-MM-DD>`
- **Last update**: `<YYYY-MM-DD>` — `<one-line state change>`
- **Cleared by**: PR `#<number>` merged at `<sha>` (fill on `shipped` / `abandoned`).
```

**Pre-flight check — every orchestrator + every delegation packet:**

1. Read this file.
2. Compute the intersection of the new slice's planned write set with every `status: claimed | in-flight` entry below.
3. **Empty intersection** → append the new claim, proceed.
4. **Non-empty intersection** → STOP. Either:
   - Coordinate with the holding slice — adjust scope, sequence behind it, or pick a different slice.
   - Promote the conflict to the user via `AskUserQuestion` with the overlap inventoried.

Agent prompts must include the lock-file path explicitly. The standard wording added to `AGENTS.md` § Orchestrator delegation packet is: *"Read `docs/design/_plan-locks.md` first. Refuse if your write set overlaps an `in-flight` or `claimed` entry; surface to the orchestrator."*

**State transitions:**

- `claimed` — orchestrator added the entry, no commits yet on the branch.
- `in-flight` — at least one commit pushed; PR may or may not be open.
- `shipped` — PR merged; entry stays for ~one merge window then prunes.
- `on-hold` — entry retained without active work; downstream slices may pre-emptively claim.
- `abandoned` — branch dropped without merge; entry pruned immediately.

**Pruning:** `shipped` entries that are older than 14 days OR whose merge sha is already in `git log origin/develop` should be deleted in the next coordination PR. Keep this file shallow.

## In-flight entries

### test-suite-expansion-completion · Phase-5-preflight · mcp-jsonrpc-pure-tu-split · status: shipped (PR #141 merged at cfab599)

- **Branch**: `feat/mcp-jsonrpc-pure-tu-split` (deleted)
- **Owner agent**: `mcp-toolsmith`
- **Originating plan**: backlog entry `2026-05-16 · mcp-toolsmith · [infra] — MCP wire-protocol pure logic entombed in cpr/httplib-tainted lambda`.
- **Claimed write set**:
  - `Plugins/Mcp/McpJsonRpcPure.h` (NEW)
  - `Plugins/Mcp/McpJsonRpcPure.cpp` (NEW)
  - `Plugins/Mcp/McpPlugin.cpp` (move-out + using-decls only; zero semantic change)
  - `CMakeLists.txt` (add `McpJsonRpcPure.cpp` to `SmatchetPlugin_Mcp` + `SmatchetPlugin_Mcp_DX12`)
  - `docs/design/_plan-locks.md`
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md`
  - `docs/design/test-suite-expansion-completion.md` (impl-log appendix)
- **Read-only adjacency**: `Plugins/Mcp/McpPlugin.h`, `Source_Core/include/SmatchetDefaults.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #141 at sha cfab599. Pure JSON-RPC surface (12 exported helpers in `smatchet::mcp::pure`) is link-clean for Phase 5 tests.
- **Cleared by**: PR `#141` merged at `cfab599`.

### test-suite-expansion-completion · Phase-5-redispatch · mcp-json-rpc-harness · status: shipped (PR #142 merged at d0b1f12)

- **Branch**: `feat/test-phase-5-mcp-json-rpc` (deleted)
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Phase 5
- **Claimed write set**:
  - `tests/Plugins/Mcp/McpRequestParser.test.cpp` (NEW)
  - `tests/Plugins/Mcp/McpEnvelope.test.cpp` (NEW)
  - `tests/Plugins/Mcp/McpToolSchemas.test.cpp` (NEW)
  - `tests/Plugins/Mcp/McpDispatch.test.cpp` (NEW)
  - `tests/CMakeLists.txt`
  - `docs/design/_plan-locks.md`
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md`
  - `docs/design/test-suite-expansion-completion.md` (impl-log appendix)
- **Read-only adjacency**: `Plugins/Mcp/McpJsonRpcPure.h`, `Plugins/Mcp/McpJsonRpcPure.cpp`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #142 at sha d0b1f12.
- **Cleared by**: PR `#142` merged at `d0b1f12`.

### test-suite-expansion-completion · Phase-6 · lua-bindings-rig · status: shipped (PR #143 merged at ba1302e)

- **Branch**: `feat/test-phase-6-lua-bindings` (merged)
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Phase 6
- **Claimed write set**: see prior entry (preserved for audit)
- **InitLuaCore classification**: Class C — `AppController_LuaBindings.cpp:32` `#include "imgui.h"` + `:766` `state["__smatchet_app"] = this` + glue functions in `smatchet_lua_init_detail::` resolve `__smatchet_app` back to live `AppController*`. Binding TU is unusable as a test link target without production refactor. LuaBindings.test.cpp deferred; sandbox + timeout + stubs-compile shipped this slice.
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — sandbox + timeout + stubs-compile shipped (14 cases / 99 assertions). LuaBindings.test.cpp deferred — unblocker (this slice) in flight.
- **Cleared by**: PR `#143` merged at `ba1302e`.

### test-suite-expansion-completion · Phase-6-unblocker · lua-bindings-host-interface-lift · status: shipped (PR #144 merged at 7e6762d)

- **Branch**: `feat/lua-bindings-host-interface-lift` (deleted)
- **Owner agent**: `lua-binder`
- **Originating plan**: backlog entry `2026-05-16 · lua-binder · [infra]` in [`docs/backlog/AGENT_SELF_IMPROVEMENT.md`](../backlog/AGENT_SELF_IMPROVEMENT.md)
- **Claimed write set**:
  - `Source_Core/include/ILuaBindingHost.h` (NEW)
  - `Source_Core/src/AppController_LuaBindingsCore.cpp` (NEW)
  - `Source_Core/src/AppController_LuaBindings.cpp` (lift out 11 glues + InitLuaCore body)
  - `Source_Core/include/AppController.h` (add `: public ILuaBindingHost` + override declarations)
  - `CMakeLists.txt` (register new TU + `-mcmodel=large` source-property)
  - `docs/design/_plan-locks.md`
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (flip top entry to applied)
  - `docs/design/test-suite-expansion-completion.md` (impl-log appendix)
- **Read-only adjacency**: `Source_Core/src/AppController_LuaStubs.cpp`, `tests/Lua/LuaSandbox.test.cpp` (sandbox closure invariant regression gate)
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #144 at sha 7e6762d. Phase 6b (LuaBindings.test.cpp round-trip) unblocked.
- **Cleared by**: PR `#144` merged at `7e6762d`.

### test-suite-expansion-completion · Phase-6b · lua-bindings-roundtrip · status: shipped (PR #145 merged at d125b36)

- **Branch**: `feat/test-phase-6b-lua-bindings-roundtrip` (deleted)
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Phase 6 (Phase 6b — Lua bindings roundtrip deferred from PR #143)
- **Claimed write set**:
  - `tests/Lua/LuaBindings.test.cpp` (NEW)
  - `tests/support/FakeLuaBindingHost.h` (NEW)
  - `tests/Lua/CMakeLists.txt` (append-only)
  - `docs/design/_plan-locks.md`
  - `docs/design/test-suite-expansion-completion.md` (impl-log appendix)
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (close any TBD PR placeholder left by PR #144)
- **Read-only adjacency**: `Source_Core/include/ILuaBindingHost.h`, `Source_Core/src/AppController_LuaBindingsCore.cpp`, `tests/support/LuaHostFixture.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #145 at sha d125b36. Phase 7 (screenshot diff) unblocked + dispatched.
- **Cleared by**: PR `#145` merged at `d125b36`.

### test-suite-expansion-completion · Phase-9 · coverage-gates · status: in-flight

- **Branch**: `feat/test-phase-9-coverage-gates`
- **Owner agent**: `build-doctor`
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Phase 9
- **Claimed write set**:
  - `scripts/dev/coverage.sh` (NEW — Windows-OpenCppCoverage-first wrapper; inline POSIX `lcov+gcov` fallback documented in header)
  - `scripts/dev/coverage-delta-gate.sh` (NEW — per-PR `Source_Core/` change without test delta → exit 1)
  - `.github/workflows/coverage.yml` (NEW — advisory coverage capture + Cobertura artifact)
  - `.github/workflows/coverage-gate.yml` (NEW — hard-blocking test-delta gate from day 1; `tests-out-of-band` label dismisses)
  - `CMakePresets.json` (append `ninja-coverage-msys2` extending `ninja-test-msys2` with gcov instrumentation)
  - `docs/design/_plan-locks.md` (this self-status flip + Phase-7 flip)
  - `docs/design/test-suite-expansion-completion.md` (impl-log + deviations + verification appendices)
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (close TBD placeholder on Phase-7 entry; optionally file follow-up entries for OpenCppCoverage CI install + threshold-flip + PR template addition)
- **Read-only adjacency**: `.github/workflows/build-and-test.yml` (pattern reference only), `cmake/Sanitizers.cmake` (helper convention reference)
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — implemented + gates green. ninja-coverage-msys2 preset configures clean; 313/313 build; ctest 2/2 PASS under instrumentation; delta-gate PASS on this slice's diff (no Source_Core/src/ touch); ninja-iter-msys2 dual-target (Standalone+DX12) clean; test-all.sh 166 passed / 8 failed (same pre-existing bucket-E exclusions noted in Phase 6/7 verification). OpenCppCoverage absent locally → coverage.sh hit the documented exit-2 install-hint branch; CI runner installs via Chocolatey. PR pending.
- **Cleared by**: TBD PR.

### test-suite-expansion-completion · Phase-7 · screenshot-diff · status: shipped (PR #146 merged at d857310)

- **Branch**: `feat/test-phase-7-screenshot-diff` (deleted)
- **Owner agent**: `test-author`
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Phase 7
- **Claimed write set**:
  - `Source_Core/include/SmatchetUiSession.h` (add `requestCommandPaletteOpen` + `requestCommandPaletteFilter` flag pair so the palette scenario can open + pre-filter the modal without touching `SmatchetUI`'s private `commandPalette_`)
  - `Source_Core/src/SmatchetUI.cpp` (consume the new flags once per frame right before `commandPalette_.Draw`)
  - `Source_Core/include/Commands/Scenarios/DockGapSentinelScenario.h` (NEW — factory entry-point)
  - `Source_Core/src/Commands/Scenarios/DockGapSentinelScenario.cpp` (NEW — drives default dock + reset, screenshot trigger)
  - `Source_Core/include/Commands/Scenarios/CommandPaletteFuzzyScenario.h` (NEW)
  - `Source_Core/src/Commands/Scenarios/CommandPaletteFuzzyScenario.cpp` (NEW)
  - `Source_Core/src/AppController.cpp` (append two `scenarioRunner_->RegisterFactory` lines next to the existing trio at lines 1290-1305)
  - `tests/support/GoldenImage.h` (NEW — header-only PPM reader + per-channel L∞ diff)
  - `tests/support/ScreenshotDiffMain.cpp` (NEW — tiny CLI helper compiled by the bash script via the host's g++ for the diff step; PascalCase to dodge the repo's `.gitignore` `screenshot_*` rule for capture outputs)
  - `tests/golden/dock-gap-sentinel.ppm` (NEW — bootstrapped via `--bootstrap` first run; 1280x720 P6 PPM)
  - `tests/golden/command-palette-fuzzy.ppm` (NEW — bootstrapped via `--bootstrap` first run; 1280x720 P6 PPM)
  - `tests/golden/README.md` (NEW — bootstrap protocol + regeneration recipe)
  - `scripts/dev/test-screenshot-diff.sh` (NEW — bash driver, auto-enrolled by test-all.sh)
  - `.github/workflows/build-and-test.yml` (append a `continue-on-error: true` screenshot-diff step that no-ops cleanly on headless runners)
  - `docs/design/_plan-locks.md` (this self-status flip)
  - `docs/design/test-suite-expansion-completion.md` (impl-log + deviations + verification appendices)
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (DX12 backbuffer readback follow-up + headless-CI display-server follow-up)
- **Read-only adjacency**: `Source_Core/src/Commands/Builtin/BuiltinCommands_Debug.cpp` (debug.window.screenshot already wired), `Source_Core/src/Commands/Builtin/BuiltinCommands_Scenario.cpp`, `Source_Core/src/Commands/Scenarios/UiTestScenario.cpp`, `Target_Standalone/main.cpp` (PPM writer already present at line 569).
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #146 at sha d857310. 2 scenarios (`dock-gap-sentinel`, `command-palette-fuzzy`) + GoldenImage.h + bash driver + advisory CI step (`continue-on-error: true` until 2026-05-30). Auto-bootstrap on first run; subsequent runs gate at L∞ <= 4.
- **Cleared by**: PR `#146` merged at `d857310`.

### test-suite-expansion · phases 2–9 · status: abandoned (superseded umbrella)

- **Originally claimed** by machine-A under the older `test-suite-expansion.md` umbrella. Superseded by `test-suite-expansion-completion.md` per-phase claims (Phase 1 / Phase 4 already shipped; Phase 5 abandoned + this pre-flight unblocker now in-flight). Pruned in coordination with the Phase 5 unblocker so the lock file reflects the real per-phase state.
- **Cleared by**: superseded — see this plan's per-phase entries.

### test-suite-expansion-completion · Phase-4 · config-schema-migration · status: shipped (PR #134 merged at 3e19f93)

- **Branch**: `feat/test-phase-4-config-migration` (deleted)
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Phase 4
- **Last update**: 2026-05-16 — merged via PR #134 at sha 3e19f93. 21 cases / 99 assertions on Config surface. Shared `tests/support/TestEnvGuard.h` shipped — Phase 5+ can consume.
- **Cleared by**: PR `#134` merged at `3e19f93`.

### test-suite-expansion-completion · Phase-5 · mcp-json-rpc-harness · status: abandoned (blocked on production TU split)

- **Branch**: `feat/test-phase-5-mcp-json-rpc` (never pushed — agent stopped before commits)
- **Owner agent**: `test-rig` (test-only scope per Phase 5 plan)
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Phase 5
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — user stopped agent at session end (wrap-up). Agent's discovery phase confirmed Phase 5 is **blocked** by the same pattern as the P4Blame deferral: every pure helper (`BuildRunLuaToolEntry`, `BuildRunLuaSummary`, `BuildToolCallSummary`, `ExtractJsonRpcErrorMessage`, `Base64Encode`, `NormalizeDomain`, `IsLoopbackAddress`, `ConstantTimeStringEquals`, `IsAllowedAttachmentHost`) lives in an anonymous namespace inside a `Plugins/Mcp/*.cpp` whose top of file pulls `winsock2` + `httplib` + `cpr`. Tests cannot link the unit without dragging banned deps. Production-side TU split needed first (same recipe as `P4BlameParse`, `TrackerLabelsPure`, etc).
- **Cleared by**: blocked — see backlog entry `2026-05-16 · mcp-toolsmith · [infra] — MCP wire-protocol logic entombed in cpr/httplib-tainted lambda`.

### backend-audit-trail-per-event-path · status: shipped

- **Branch**: `feat/audit-trail-per-event-path`
- **Owner agent**: orchestrator
- **Originating plan**: backlog entries 2026-05-16 `security-review` + `offline-sync` in [`docs/backlog/AGENT_SELF_IMPROVEMENT.md`](../backlog/AGENT_SELF_IMPROVEMENT.md)
- **Claimed write set**:
  - `Source_Core/src/BackendAuditTrail.cpp` (writer re-resolves `GetAuditFilePath()` per-event)
  - `tests/Source_Core/BackendAuditTrail.test.cpp` (add runtime-dir-change case; existing TEST_CASE workaround drops)
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (status flip)
- **Read-only adjacency**: `Source_Core/include/BackendAuditTrail.h`, `Source_Core/include/ConfigManager.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #108 at sha 98ed9ea.
- **Cleared by**: PR `#108` merged at `98ed9ea`.

### cached-ticket-types-header-split · status: shipped

- **Branch**: `feat/cached-ticket-types-header-split`
- **Owner agent**: orchestrator
- **Originating plan**: backlog entry 2026-05-16 `test-rig · [infra]` in [`docs/backlog/AGENT_SELF_IMPROVEMENT.md`](../backlog/AGENT_SELF_IMPROVEMENT.md)
- **Claimed write set**:
  - `Source_Core/include/CachedTicketTypes.h` (NEW)
  - `Source_Core/include/LocalCacheManager.h`
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md`
- **Read-only adjacency**: 20 callers of `LocalCacheManager.h` (no code edit — re-include keeps API)
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — shipped on develop at sha 8724b8b (branch deleted).
- **Cleared by**: merged to develop at `8724b8b`.

### code-review-backlog · A3 · required-field-glyph · status: in-flight

- **Branch**: `feat/required-field-ui-glyph`
- **Owner agent**: `tracker-backend`
- **Originating plan**: [`backlog/BACKLOG_CODE_REVIEW.md`](../../backlog/BACKLOG_CODE_REVIEW.md) § A3
- **Claimed write set**:
  - `Source_Core/src/TicketFieldEditor.cpp`
  - `Source_Core/src/SmatchetNewIssueDraftUi.cpp`
  - `Source_Core/src/SmatchetLocalization.cpp` (agent corrected: baseline strings live in C++ table, not `Locales/*.json`)
  - `backlog/BACKLOG_CODE_REVIEW.md` (status flip A3)
- **Read-only adjacency**: `Source_Core/include/TrackerFieldCatalog.h`, `Source_Core/include/TicketFieldEditor.h`, `Source_Core/include/SmatchetNewIssueDraftUi.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — PR [#113](https://github.com/alexandrosk0/Smatchet/pull/113) opened against `develop`.
- **Cleared by**: pending merge of [#113](https://github.com/alexandrosk0/Smatchet/pull/113).

### code-review-backlog · C7 · grid-pushcliprect-audit · status: in-flight

- **Branch**: `feat/grid-pushcliprect-audit`
- **Owner agent**: `grid-engine`
- **Originating plan**: [`backlog/BACKLOG_CODE_REVIEW.md`](../../backlog/BACKLOG_CODE_REVIEW.md) § C7
- **Claimed write set**:
  - `Source_Core/src/SmatchetActiveProjectGridUi.cpp` (lines 843, 905, 930 — remove `PushClipRect`/`PopClipRect` pairs if redundant)
  - `backlog/BACKLOG_CODE_REVIEW.md` (status flip C7)
- **Read-only adjacency**: `Source_Core/include/SmatchetActiveProjectGridUi.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — PR [#115](https://github.com/alexandrosk0/Smatchet/pull/115) opened against `develop`.
- **Cleared by**: pending merge of [#115](https://github.com/alexandrosk0/Smatchet/pull/115).

### code-review-backlog · C2 · markdown-emitinlinetext-scratch · status: in-flight

- **Branch**: `feat/markdown-emitinlinetext-scratch`
- **Owner agent**: orchestrator (direct — small allocator-only change, no header touch)
- **Originating plan**: [`backlog/BACKLOG_CODE_REVIEW.md`](../../backlog/BACKLOG_CODE_REVIEW.md) § C2
- **Claimed write set**:
  - `Source_Core/src/MarkdownConvert.cpp` (EmitInlineText at line 723 only)
  - `backlog/BACKLOG_CODE_REVIEW.md` (status flip C2)
- **Read-only adjacency**: none.
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — PR [#117](https://github.com/alexandrosk0/Smatchet/pull/117) opened against `develop`.
- **Cleared by**: pending merge of [#117](https://github.com/alexandrosk0/Smatchet/pull/117).

### code-review-backlog · C3 · plane-fetchissueeditmeta-broaden · status: in-flight

- **Branch**: `feat/plane-fetchissueeditmeta-broaden`
- **Owner agent**: orchestrator (direct — one-line list expansion)
- **Originating plan**: [`backlog/BACKLOG_CODE_REVIEW.md`](../../backlog/BACKLOG_CODE_REVIEW.md) § C3
- **Claimed write set**:
  - `Source_Core/src/PlaneFieldCatalog.cpp` (FetchIssueEditMeta at line 492 only — broaden hardcoded set; root cause "Plane has no per-issue capability endpoint" documented inline)
  - `backlog/BACKLOG_CODE_REVIEW.md` (status flip C3 → 🟡 partial; real-permissions query deferred)
- **Read-only adjacency**: `Source_Core/include/PlaneClient.h`.
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — PR [#118](https://github.com/alexandrosk0/Smatchet/pull/118) opened against `develop`.
- **Cleared by**: pending merge of [#118](https://github.com/alexandrosk0/Smatchet/pull/118).

### code-review-backlog · B4 · plane-fetchissuesforkeys-filter · status: in-flight

- **Branch**: `feat/plane-fetchissuesforkeys-filter`
- **Owner agent**: `tracker-backend` (orchestrator-direct after isolated worktrees thrashed twice on API 500)
- **Originating plan**: [`backlog/BACKLOG_CODE_REVIEW.md`](../../backlog/BACKLOG_CODE_REVIEW.md) § B4
- **Claimed write set**:
  - `Source_Core/src/PlaneIssueSearch.cpp` (FetchIssuesForKeys only — file split from `PlaneClient.cpp` since the backlog entry was written)
  - `backlog/BACKLOG_CODE_REVIEW.md` (status flip B4 → 🟡 partial; server-side `sequence_id__in` filter deferred as B4-v2)
- **Read-only adjacency**: `Source_Core/include/PlaneClient.h`, `Source_Core/include/ITrackerClient.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — PR [#116](https://github.com/alexandrosk0/Smatchet/pull/116) opened against `develop`.
- **Cleared by**: pending merge of [#116](https://github.com/alexandrosk0/Smatchet/pull/116).

### test-suite-expansion-completion · wave-A1 · callstack-adversarial-subcases · status: shipped

- **Branch**: `feat/test-callstack-adversarial`
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Carry-over A
- **Claimed write set**:
  - `tests/Source_Core/CallstackParser.test.cpp`
  - `docs/design/applied/test-suite-expansion.md` (impl-log appendix)
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (status flip on `code-review+security-review · [test]` entry)
- **Read-only adjacency**: `Source_Core/src/CallstackParser.cpp`, `Source_Core/include/CallstackParser.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #112 at sha effda92.
- **Cleared by**: PR `#112` merged at `effda92`.

### test-suite-expansion-completion · wave-A1 · p4blame-parse-tu-split · status: shipped

- **Branch**: `feat/p4blame-parse-tu-split`
- **Owner agent**: `test-rig` (TU-split pre-authorised per AGENTS.md applied rule)
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Carry-over B
- **Claimed write set**:
  - `Source_Core/include/P4BlameParse.h` (NEW)
  - `Source_Core/src/P4BlameParse.cpp` (NEW)
  - `Source_Core/src/P4Blame.cpp` (call-site rewire of the four lifted helpers only — no semantic change)
  - `tests/Source_Core/P4BlameParse.test.cpp` (NEW)
  - `tests/CMakeLists.txt`
  - `docs/design/applied/test-suite-expansion.md` (impl-log appendix)
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (status flip on `test-rig · [infra] — Phase 2 P4BlameParse deferred`)
- **Read-only adjacency**: `Source_Core/include/P4Blame.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #111 at sha 52832d0.
- **Cleared by**: PR `#111` merged at `52832d0`.

### test-suite-expansion-completion · wave-A2 · tracker-labels-pure-tu · status: shipped (PR #114 merged at 59282a7)

- **Branch**: `feat/tracker-labels-pure-tu`
- **Owner agent**: `test-rig` (TU-split pre-authorised)
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Carry-overs C1
- **Claimed write set**:
  - `Source_Core/include/TrackerLabelsPure.h` (NEW)
  - `Source_Core/src/TrackerLabelsPure.cpp` (NEW)
  - `Source_Core/src/TrackerLabelsEditor.cpp` (call-site rewire of pure helpers only — no semantic change)
  - `tests/Source_Core/TrackerLabelsPure.test.cpp` (NEW)
  - `tests/CMakeLists.txt`
  - `docs/design/applied/test-suite-expansion.md` (impl-log appendix)
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (status flip on Phase 1 deferral entry)
  - `docs/design/_plan-locks.md` (self-status flips)
- **Read-only adjacency**: `Source_Core/include/TrackerLabelsEditor.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — agent errored API-500 mid-run; orchestrator recovered worktree state, verified gates, committed + pushed + merged.
- **Cleared by**: see PR table below.

### test-suite-expansion-completion · wave-A2 · tracker-datetime-pure-tu · status: shipped (PR #119 merged at 9fc5f70)

- **Branch**: `feat/tracker-datetime-pure-tu`
- **Owner agent**: `test-rig` (TU-split pre-authorised)
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Carry-overs C2
- **Claimed write set**:
  - `Source_Core/include/TrackerDateTimePure.h` (NEW)
  - `Source_Core/src/TrackerDateTimePure.cpp` (NEW)
  - `Source_Core/src/TrackerDateTimeFieldEditor.cpp` (call-site rewire only)
  - `tests/Source_Core/TrackerDateTimePure.test.cpp` (NEW)
  - `tests/CMakeLists.txt`
  - `docs/design/applied/test-suite-expansion.md` (impl-log appendix)
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (status flip on Phase 1 deferral entry)
  - `docs/design/_plan-locks.md` (self-status flips)
- **Read-only adjacency**: `Source_Core/include/TrackerDateTimeFieldEditor.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — agent errored API-500 mid-run; orchestrator recovered worktree state, verified gates, committed + pushed + merged.
- **Cleared by**: see PR table below.

### test-suite-expansion-completion · wave-A2 · tracker-payload-pure-tu · status: shipped (PR #121 merged at 39f91de)

- **Branch**: `feat/tracker-payload-pure-tu`
- **Owner agent**: `test-rig` (TU-split pre-authorised)
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Carry-overs C3
- **Claimed write set**:
  - `Source_Core/include/TrackerFieldPayloadPure.h` (NEW)
  - `Source_Core/src/TrackerFieldPayloadPure.cpp` (NEW)
  - `Source_Core/src/TrackerFieldPayload.cpp` (call-site rewire only — `JiraClient.h` stays in production TU)
  - `tests/Source_Core/TrackerFieldPayloadPure.test.cpp` (NEW)
  - `tests/CMakeLists.txt`
  - `docs/design/applied/test-suite-expansion.md` (impl-log appendix)
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (status flip on Phase 1 deferral entry)
  - `docs/design/_plan-locks.md` (self-status flips)
- **Read-only adjacency**: `Source_Core/include/TrackerFieldPayload.h`, `Source_Core/include/JiraClient.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — agent errored API-500 mid-run; orchestrator recovered worktree state, verified gates, committed + pushed + merged.
- **Cleared by**: see PR table below.

### test-suite-expansion-completion · wave-A2 · tracker-field-catalog-pure-tu · status: shipped (PR #122 merged at 5ce8def)

### test-suite-expansion-completion · PR-D · offline-queue-deps-interface · status: shipped (PR #127 merged at b5fc194)

- **Branch**: `feat/offline-queue-deps-interface` (deleted)
- **Owner agent**: `offline-sync`
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Per-slice scoping § PR D
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #127 at sha b5fc194. Track B (`large-files-and-phase-2`) on-hold gate now releases.
- **Cleared by**: PR `#127` merged at `b5fc194`.

### test-suite-expansion-completion · PR-E · offline-queue-runtime-tests · status: shipped (PR #131 merged at e35794d)

- **Branch**: `feat/offline-queue-runtime-tests` (deleted)
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Per-slice scoping § PR E
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #131 at sha e35794d. Required orchestrator-side dedup after rebase: test-side `IsTrackerTransportErrorText` mirror collided with production once PR F's `TrackerHttpUtils.cpp` joined the test target source list.
- **Cleared by**: PR `#131` merged at `e35794d`.

### test-suite-expansion-completion · PR-F · ticket-sync-service-tests · status: shipped (PR #130 merged at a618a2f)

- **Branch**: `feat/ticket-sync-service-tests` (deleted)
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Per-slice scoping § PR F
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #130 at sha a618a2f. 12 cases / 83 assertions. Case 3 documents a current production bug (empty fetch in full-sync deletes all rows) — separate fix-PR pending under `offline-sync` follow-up (backlog entry filed).
- **Cleared by**: PR `#130` merged at `a618a2f`.

- **Branch**: `feat/tracker-field-catalog-pure-tu`
- **Owner agent**: `test-rig` (TU-split pre-authorised)
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Carry-overs C4
- **Claimed write set**:
  - `Source_Core/include/TrackerFieldCatalogPure.h` (NEW)
  - `Source_Core/src/TrackerFieldCatalogPure.cpp` (NEW)
  - `Source_Core/src/TrackerFieldCatalog.cpp` (call-site rewire only — `JiraClient.h` stays in production TU)
  - `tests/Source_Core/TrackerFieldCatalogPure.test.cpp` (NEW)
  - `tests/CMakeLists.txt`
  - `docs/design/applied/test-suite-expansion.md` (impl-log appendix)
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (status flip on Phase 1 deferral entry)
  - `docs/design/_plan-locks.md` (self-status flips)
- **Read-only adjacency**: `Source_Core/include/TrackerFieldCatalog.h`, `Source_Core/include/JiraClient.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — agent errored API-500 mid-run; orchestrator recovered worktree state, verified gates, committed + pushed + merged.
- **Cleared by**: see PR table below.

### test-suite-expansion · phase 1 · status: in-flight

- **Branch**: `feat/test-phase-1-tracker-pure-logic`
- **Owner agent**: orchestrator (autonomous multi-phase mode per the plan's § Execution contract)
- **Originating plan**: [`docs/design/applied/test-suite-expansion.md`](./applied/test-suite-expansion.md) § Phase 1
- **Claimed write set**:
  - `Source_Core/include/IssueCreatePipelineHelpers.h` (NEW)
  - `Source_Core/src/IssueCreatePipeline.cpp`
  - `Source_Core/src/IssueCreatePipelineHelpers.cpp` (NEW)
  - `Source_Core/src/IssueDraft.cpp`
  - `Source_Core/src/TrackerFieldValueParser.cpp`
  - `tests/CMakeLists.txt`
  - `tests/Source_Core/IssueCreatePipeline.test.cpp` (NEW)
  - `tests/Source_Core/IssueDraft.test.cpp` (NEW)
  - `tests/Source_Core/TrackerFieldValueParser.extended.test.cpp` (NEW)
  - `tests/Source_Core/TrackerFieldValueUtils.test.cpp` (NEW)
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md`
  - `docs/design/applied/test-suite-expansion.md` (impl-log appendix)
- **Read-only adjacency**: `Source_Core/include/IssueDraft.h`, `Source_Core/include/TrackerFieldValueParser.h`, `Source_Core/include/IssueCreatePipeline.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — PR `#103` opened against `develop`.
- **Cleared by**: pending merge of [#103](https://github.com/alexandrosk0/Smatchet/pull/103).

### test-suite-expansion · phases 2–9 · status: abandoned (superseded by `test-suite-expansion-completion.md` per-phase claims)

- **Original Owner**: orchestrator (autonomous; see [`docs/design/applied/test-suite-expansion.md`](./applied/test-suite-expansion.md) § Execution contract).
- **Superseded by**: Phase-by-phase claims under `test-suite-expansion-completion.md`. Phases 1 + 4 shipped; Phase 5 abandoned then unblocked by the `mcp-jsonrpc-pure-tu-split` slice above. The umbrella claim was never honoured across the 8 phases that actually shipped against develop.
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — converted `claimed` → `abandoned` by the Phase-5 pre-flight unblocker so the lock file matches reality.
- **Cleared by**: superseded.

### large-files-and-phase-2 · Track B (B1–B3 + fix-up) · status: on-hold

- **Branch**: TBD per slice (B1: `claude/offline-queue-icache-access`, B2: TBD, B3: TBD)
- **Owner agent**: `offline-sync` (B1, B2), `lua-binder` (B3)
- **Originating plan**: [`docs/design/applied/large-files-and-phase-2.md`](./applied/large-files-and-phase-2.md) § Track B
- **Reason on-hold**: overlapping write set with `test-suite-expansion` phases 2-9 (`TicketSyncService.cpp`, `ConfigManager.cpp`, `AppController.h`, `tests/CMakeLists.txt`). Resuming Track B before those test phases land would force a multi-way rebase that defeats both efforts.
- **Resume gate**: `test-suite-expansion` § Implementation log shows phase 9 shipped, OR the user explicitly green-lights an earlier resume with a narrower-than-umbrella write set.
- **Claimed write set on resume** (preview — re-asserted at resume time):
  - B1: `Source_Core/include/ICacheAccess.h` (NEW), `Source_Core/include/AppController.h`, `Source_Core/src/AppController.cpp`, `Source_Core/src/AppController_*.cpp` (rename `Cache` → `cache_`), `Source_Core/include/OfflineQueueService.h`, `Source_Core/src/OfflineQueueService.cpp`, `tests/CMakeLists.txt` (if rename cascades to test-rig-compiled sources)
  - B2: `Source_Core/include/ITicketSyncHost.h` (NEW), `Source_Core/include/AppController.h`, `Source_Core/src/AppController.cpp`, `Source_Core/include/TicketSyncService.h`, `Source_Core/src/TicketSyncService.cpp`
  - B3a-d: `Source_Core/include/LuaAutomationHost.h`, `Source_Core/src/LuaAutomationHost.cpp`, `Source_Core/src/AppController_LuaBindings.cpp`, `Source_Core/src/AppController_LuaStubs.cpp`, `Source_Core/include/AppController.h`, `Source_Core/src/AppController.cpp` (Lua state + worker thread + Phase-2 `ITrackerActions` interface)
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — held by user after parallel `test-suite-expansion` plan surfaced. Track A (5 mechanical splits) already shipped; Track B paused at the boundary.
- **Cleared by**: TBD (gate above).

### ai-assistant-side-panel · Phase A-narrowed · status: in-flight

- **Branch**: `feat/ai-assistant-side-panel` (plan recovered from dangling 84913a8 → a39097c)
- **Owner agent**: orchestrator (direct — provider-pluggable C++14 skeleton)
- **Originating plan**: [`docs/design/ai-assistant-side-panel.md`](./ai-assistant-side-panel.md) § Phase A
- **Claimed write set** (narrowed — ConfigManager fields + tests deferred to Phase A' until `test-suite-expansion` umbrella releases `Source_Core/src/ConfigManager*.cpp` + `tests/**`):
  - `Source_Core/include/IAiClient.h` (NEW)
  - `Source_Core/include/AiTypes.h` (NEW)
  - `Source_Core/include/AiClientFactory.h` (NEW)
  - `Source_Core/src/AiClientFactory.cpp` (NEW)
  - `Source_Core/include/OpenAiClient.h` (NEW)
  - `Source_Core/src/OpenAiClient.cpp` (NEW)
  - `Source_Core/include/AiSseParser.h` (NEW)
  - `Source_Core/src/AiSseParser.cpp` (NEW)
  - `Source_Core/include/NetworkUsageTracker.h` (re-add `HttpTrafficKind::Ai` + `aiRequests/aiUploadBytes/aiDownloadBytes` snapshot fields)
  - `Source_Core/src/NetworkUsageTracker.cpp`
  - `Source_Core/src/TrackerHttpUtils.cpp` (one-line `Record(HttpTrafficKind::Tracker, …)` update)
  - `Source_Core/src/JiraIssueMutation.cpp` (one-line `Record` update)
  - `Source_Core/src/FieldCatalogCache.cpp` (one-line `Record` update)
  - `CMakeLists.txt` (add `option(SMATCHET_WITH_AI "..." ON)` + new sources to `CORE_SOURCES`)
- **Read-only adjacency**: `Source_Core/include/AppController.h`, `Source_Core/include/MainThreadDispatcher.h`
- **Deferred to Phase A' (gated on test-suite-expansion umbrella release)**:
  - `Source_Core/include/ConfigManager.h` + `Source_Core/src/ConfigManager.cpp` (Ai field set + DPAPI key protection)
  - `tests/CMakeLists.txt` + `tests/Source_Core/AiSseParser.test.cpp` + `tests/Source_Core/AiClientFactory.test.cpp` (doctest coverage)
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — Phase A code committed locally (skeleton + OpenAiClient + NetworkUsageTracker re-fit + CMake option/shim). Both targets build clean. Push + PR pending. Phases B-E unscoped pending Phase A ship + Track-B / umbrella status check.
- **Cleared by**: TBD.

## Shipped recent entries

(Pruned after ~14 days. Kept here briefly to give concurrent agents recent context.)

### large-files-and-phase-2 · Track A · status: shipped

- A5 → PR [#93](https://github.com/alexandrosk0/Smatchet/pull/93) (merged 2026-05-16)
- A3 → PR [#97](https://github.com/alexandrosk0/Smatchet/pull/97) (merged 2026-05-16)
- A2 → PR [#98](https://github.com/alexandrosk0/Smatchet/pull/98) (merged 2026-05-16)
- A1 → PR [#100](https://github.com/alexandrosk0/Smatchet/pull/100) (pending)
- A4 → PR [#101](https://github.com/alexandrosk0/Smatchet/pull/101) (pending)
- Plan revision → PR [#102](https://github.com/alexandrosk0/Smatchet/pull/102) (pending)

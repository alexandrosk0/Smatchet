# Test suite expansion — phased autonomous roadmap

## Context

Smatchet ships a thin test surface relative to the size and risk of the app:

- **7 doctest units** (~194 `CHECK`s) covering JqlProjectScope, JsonParseUtil, MarkdownPreviewLangTag, OfflineQueueReplayPolicy, ThemeSyntaxColors, TextMerge, TrackerFieldValueParser (partial).
- **2 ImGui Test Engine** tests (norton_commander_theme, views_columns_reorder).
- **9 dev shell scripts** under `scripts/dev/test-*.sh` aggregated by `scripts/dev/test-all.sh`.
- **No HTTP mock, no in-memory SQLite fixture, no screenshot diff, no MCP wire-protocol harness, no Unreal smoke.**

Untested high-risk surface:

| Surface | Risk if it breaks |
|---|---|
| IssueCreatePipeline (`MergeDraftIntoCachedTicketForUpdate`, `ApplyPostIssueSteps`, `RunUpdateExisting`) | Lost edits, wrong JSON shipped to Jira / Plane, broken post-create transitions |
| IssueDraft (`FromCachedTicket`, `PruneUnchangedFields`, `ToJson` round-trip) | Offline queue corruption — replay sends wrong fields |
| TrackerFieldValueParser (`MergeTrackerFieldOption`, `ClassifyTrackerFieldFamily`, `NormalizeTrackerFieldValue`) | Option-list corruption, wrong field-family routing, wrong display |
| CallstackParser (`TryParsePathLinePair`, `ApplyPathRemaps`) | Wrong file:line in blame; users open wrong source |
| LocalCacheManager (`SaveTicket` transaction) | DB corruption mid-write — losing edits silently |
| TicketSyncService (`ApplyIssueFetchPack`) | Full-sync delete-stale logic — wipes tickets if fetch is partial |
| ConfigManager migration | Wrong schema after upgrade, lost config |
| MCP JSON-RPC layer | Wire-protocol regressions invisible until clients reconnect |
| Lua sandbox / timeout | Script can hang UI or escape sandbox |
| Unreal embedding (`SMATCHET_EMBEDDED_IN_UNREAL`) | Header pollution + plugin packaging regressions ship to UE host invisibly |

Goal: get every high-risk surface under a deterministic, fast, headless regression gate with **zero manual residue**.

---

## Execution contract — autonomous mode

This plan is executed by the orchestrator **without user input** between phases. The orchestrator owns the loop end-to-end:

```
for phase in 1..9:
    git checkout develop && git pull
    git checkout -b feat/test-phase-{N}-{slug}
    delegate to subsystem agent(s) per phase table
    run local gate: cmake --build ... && ctest ... && scripts/dev/test-all.sh
    if local gate red:
        attempt self-repair (re-run failing test, read log, fix, repeat — up to 3 cycles)
        if still red:
            isolate failing case to a deferred-test entry in docs/backlog/AGENT_SELF_IMPROVEMENT.md
            disable the case with [.skip] tag + backlog cross-ref
            continue (do NOT halt)
    git add . && git commit -m "test(phase-{N}): <slug>"
    git push -u origin feat/test-phase-{N}-{slug}
    gh pr create --base develop --title "test(phase-{N}): <slug>" --body <body>
    invoke code-review + security-review agents in parallel on the PR diff
    apply auto-fixable findings; defer non-blocking findings to backlog
    wait for CI green (ninja-test-msys2 + build-and-test workflows)
    gh pr merge --squash --delete-branch --auto
    append shipped commit sha + summary to docs/design/test-suite-expansion.md § Implementation log
    proceed to next phase
```

### Blocking vs non-blocking — decision table

| Symptom | Class | Action |
|---|---|---|
| Build fails (compile / link error) | **Blocking** | `build-doctor` agent diagnoses; fix in-PR before merge |
| Existing test newly red on develop | **Blocking** | `debug-detective` agent; fix in-PR |
| New test flakes (one of N runs fails) | **Non-blocking** | mark `[.flaky]` + backlog entry (category `tooling`); continue |
| New test reveals real prod bug | **Non-blocking** | open separate `fix:` PR after current phase merges; backlog entry to track; do not block test PR |
| Interface refactor needed (e.g. extract `ITrackerHttpClient`) larger than 1 file | **Non-blocking** | defer the affected test cases to a follow-up phase via backlog; ship what can be tested without the refactor |
| CI runner missing capability (Mesa GL, UE install) | **Non-blocking** | mark phase gate `advisory` + backlog entry; continue |
| Production code lacks an obvious testable seam | **Non-blocking** | backlog entry under category `context`; move on |
| Security-review finding in **test infra** | Blocking if it touches secrets / network egress; otherwise non-blocking | judgement; default non-blocking for test-only code |

**Halt-only conditions** (very rare):
- `develop` is dirty with user-authored uncommitted work (per AGENTS.md). Run `git status` between phases — if dirty, stash with a clear name + post a chip and continue rather than abort.
- Force-push to `develop` would be required to proceed — never; backlog the entire phase, move on.
- Sanitizer (ASan / UBSan) fails on existing code — that's a pre-existing prod bug; route to `debug-detective` as a separate PR while continuing test work.

### PR contract

Every phase PR uses this template:

```
test(phase-{N}): <slug>

## Summary
- One-line per new test file or fixture
- Coverage delta (new TEST_CASEs, new CHECKs)

## Test plan
- [x] ctest --preset ninja-test-msys2 green
- [x] scripts/dev/test-all.sh green
- [x] new tests fail under documented mutation (one-off; not in CI)

## Deferrals
- (List of backlog entries created during this phase, if any)

🤖 Generated with [Claude Code](https://claude.com/claude-code)
```

### Auto-merge mechanics

- `gh pr merge --squash --delete-branch --auto` is fired immediately after CI turns green and `code-review` / `security-review` finish.
- Branch protection on `develop` requires a green CI check + one approving review. Use `--admin` only if branch protection is configured to allow it; otherwise rely on the bot review.
- Squash-merge keeps history linear and matches the existing repo convention (`fc5e469`, `74472f1`, etc.).
- Janitor sweep (`git-janitor` agent) runs after Phase 9 lands.

### Deferral protocol — `docs/backlog/AGENT_SELF_IMPROVEMENT.md`

Every non-blocking issue gets exactly one entry:

```
- 2026-MM-DD · test-suite-expansion · [<category>] — <one-line description>.
  **Blocker**: <what's missing>.
  **Workaround**: <current state — usually `[.skip]` tag in test or advisory CI gate>.
  **Est. cost**: <rough hours>.
```

Categories: `tooling`, `infra`, `context`, `feature`, `test`. Never `bug` from test-suite work — those go to a separate PR.

---

## Locked defaults — no clarifications needed

These were open questions in the previous revision. They are **decided** now, so execution does not stall:

| Decision | Locked value | Reversal trigger |
|---|---|---|
| `TrackerHttpClient` interface | If no interface exists, **promote in same PR as Phase 3** under `tracker-backend` agent; behavior-only refactor (no semantics change). If the diff exceeds 200 LoC or touches `SmatchetCore_DX12`, defer the extraction to a follow-up PR via backlog and ship Phase 3 against the existing concrete class via a thin shim. | `unreal-bridge` agent reports DX12 build break |
| GL context in CI (Phase 7) | **Windows runner only**; Mesa llvmpipe not assumed. Pure-headless CI runner skips screenshot diff with a yellow advisory mark. | Mesa later ships on the runner — flip to required |
| UE version (Phase 8) | **UE 5.4** as default. If `unreal-bridge` agent reports a different shipped version, use that. If no UE install is reachable from the build environment, **defer Phase 8** with a backlog entry and proceed to Phase 9. | Smatchet officially pins a UE version |
| Coverage tool (Phase 9) | **`OpenCppCoverage` on Windows**. `lcov + gcov` only as documented fallback in `scripts/dev/coverage.sh` (auto-selected if OpenCppCoverage exits non-zero on init). | OpenCppCoverage breaks on the runner |
| Config schema fixtures (Phase 4) | Use **the current shipped schema version only** plus scaffold for the next bump. If only one version exists, Phase 4 still ships validity + hostile-input + round-trip cases. | A migration is added later |
| Mutation testing | **Out of scope.** Phase 10 backlog entry only — never autostart. | Manual decision |
| `IssueCreatePipeline` pure-helper extraction (Phase 1) | If the helpers can be `static`-promoted without touching headers, do it in the test PR. Otherwise, defer the entangled helpers via a backlog entry and ship tests for the cleanly-pure subset. **Never block Phase 1 on the refactor.** | `code-review` flags shape change |
| `IScenario` / scenario authoring (Phase 7-8) | Reuse existing pattern in `Source_Core/include/Commands/Scenarios/IScenario.h` (see `PriorityGridScrollScenario.cpp`). No new scenario framework. | — |
| `ImGui Test Engine` (already wired) | Extend with **at least one new test per phase that touches UI surface** (Phase 7, 8). Bucket E is the assertion home for UI behavior. | — |

---

## Approach

Nine phases. Each phase is one PR (or 2-3 if the diff exceeds ~600 LoC + tests). Phases run **sequentially** by default to keep `develop` cleanly bisectable. The orchestrator may run Phases 1 and 2 in parallel (disjoint write sets) when wall-clock matters.

| Phase | Topic | New infra? | Approx effort | Local gate |
|---|---|---|---|---|
| 1 | Tracker mutation + parser pure-logic | No | 2-3 d | doctest |
| 2 | Callstack + P4 blame parsing | No | 0.5 d | doctest |
| 3 | HTTP mock + in-memory SQLite fixture (+ first cache/queue tests) | **Yes** | 2 d | doctest |
| 4 | Config + schema migration | No | 1 d | doctest + shell |
| 5 | MCP JSON-RPC pure-logic harness | **Yes (light)** | 1 d | doctest |
| 6 | Lua full binding round-trip rig | **Yes (links real lua_State)** | 1.5 d | doctest |
| 7 | Screenshot diff (bucket C) | **Yes** | 1 d | scenario + golden PPM |
| 8 | Unreal headless smoke | **Yes (UE project + CI matrix)** | 2-3 d | UE automation spec (advisory) |
| 9 | Coverage gates + enforcement | **Yes (OpenCppCoverage + CI gate)** | 0.5 d | CI policy |

**Order rationale**: pure-logic doctests first (no infra, instant value). Infra phases ship with their first batch of consuming tests so the harness lands battle-tested. UE smoke late because CI cost is real and the project structure is the riskiest. Coverage gates last so the gate operates on a real baseline.

---

## Cross-cutting conventions

- **Test layout**: `tests/Source_Core/<Unit>.test.cpp`, registered in `tests/CMakeLists.txt`, run by `SmatchetTests` doctest binary via `ninja-test-msys2` preset.
- **Support code** under `tests/support/` (new dir): headers only when possible.
- **Naming**: `TEST_CASE("<Unit>: <behavior>")`, `SUBCASE` for table-driven cases. Tag `[high-risk]` on cases mapped to documented incidents / known footguns. Tag `[.skip]` (with backlog cross-ref in the message) on cases parked due to non-blocking deferral.
- **Mutation budget** (one-off, not CI): every new high-risk `TEST_CASE` must demonstrably fail under a documented invariant flip. Recorded in the PR description.
- **Env isolation**: integration / scenario tests set `SMATCHET_USER_DATA` to a `mktemp -d` per script and `rm -rf` on exit.
- **Plan revision**: each PR appends `## Implementation log`, `## Deviations from plan`, `## Verification` entries to this file in the same commit.
- **AGENTS.md UX Pillars**: every new fixture / scenario asserts pillar invariants where applicable.
- **Delegation**:
  - Phases 1-2, 4 — `test-rig` (pure doctest).
  - Phase 3 — `test-rig` + `tracker-backend` + `offline-sync` (interface extraction + fixtures).
  - Phase 5 — `test-rig` + `mcp-toolsmith`.
  - Phase 6 — `test-rig` + `lua-binder`.
  - Phase 7 — `test-author` + `command-system` (new CLI command).
  - Phase 8 — `test-author` + `unreal-bridge` + `build-doctor`.
  - Phase 9 — `build-doctor` + `test-author`.
  - Each phase: `code-review` + `security-review` in parallel on the PR diff before auto-merge.

---

## Phase 1 — Tracker mutation + parser pure-logic doctests

**Goal**: cover every pure function in the tracker mutation / parsing path that can corrupt data shipped to Jira / Plane or break offline queue replay.

**Branch**: `feat/test-phase-1-tracker-pure-logic`.

**No new infra.** Add `TEST_CASE`s to existing doctest rig.

### Files to add under `tests/Source_Core/`

| New test file | Covers | Key cases |
|---|---|---|
| `IssueDraft.test.cpp` | `Source_Core/src/IssueDraft.cpp` | `ResolveIssueTypeIdFromTicket` · `FromCachedTicket` · `MissingRequiredFields` · `ToJson` ↔ `FromJson` round-trip (incl. number-vs-string drift) · `ComputeFieldChanges` · `PruneUnchangedFields` (cache-miss safety) |
| `IssueCreatePipeline.test.cpp` | pure helpers from `Source_Core/src/IssueCreatePipeline.cpp:49-298` | `MergeDraftIntoCachedTicketForUpdate` driven by synthetic `putFieldsSucceeded` JSON · `ApplyPostIssueSteps` decision logic (status replay, sprint replay, conditional skip). Pure helpers promoted to `static` in same TU **only if** no header change needed; otherwise defer entangled helpers via backlog. |
| `TrackerFieldValueParser.extended.test.cpp` | `Source_Core/src/TrackerFieldValueParser.cpp` | `BuildTrackerOptionDisplayValue` / `BuildTrackerOptionId` priority fallback · `TrackerFieldOptionFromJson` · **`MergeTrackerFieldOption` [high-risk]** · `ClassifyTrackerFieldFamily` · `NormalizeTrackerFieldValue` · `ParseComments` · `ParseChangelog` |
| `TrackerFieldValueUtils.test.cpp` | `Source_Core/src/TrackerFieldValueUtils.cpp` | every public helper |
| `TrackerFieldValuePayload.test.cpp` | `Source_Core/src/TrackerFieldValuePayload.cpp` | payload per field-family (string / number / option / multi-option / user / cascading) |
| `TrackerLabelsEditor.test.cpp` | `Source_Core/src/TrackerLabelsEditor.cpp` | parse / serialize round-trip; whitespace; dup detection |
| `TrackerDateTimeFieldEditor.test.cpp` | `Source_Core/src/TrackerDateTimeFieldEditor.cpp` | input → ISO-8601; rejected formats; tz handling |
| `JqlSurgery.test.cpp` | `Source_Core/src/JqlSurgery.cpp` | full surgery surface beyond project-scope: ORDER BY rewrite, status filter injection, LIKE-escaping |
| `TrackerFieldCatalog.test.cpp` | `Source_Core/src/TrackerFieldCatalog.cpp` pure parts | catalog merge, field-id lookup, allowed-values filtering |

### Cases gated `[high-risk]`

- `IssueDraft.test.cpp::PruneUnchangedFields_DoesNotEraseOnCacheMiss`
- `IssueDraft.test.cpp::ToJsonFromJson_RoundTripPreservesNumericTypes`
- `IssueCreatePipeline.test.cpp::MergeDraftIntoCachedTicketForUpdate_HonoursPutFieldsSucceeded`
- `TrackerFieldValueParser.extended.test.cpp::MergeTrackerFieldOption_DoesNotDuplicateById`
- `TrackerFieldValueParser.extended.test.cpp::ClassifyTrackerFieldFamily_DetectsCascadingSelect`

### Refactor handling

If pure-helper extraction in `IssueCreatePipeline.cpp` requires changing a public header, **abort the extraction**, log a backlog entry, and ship only the tests that work against the current shape. Do not stall Phase 1.

### Local gate

```bash
cmake --build --preset ninja-test-msys2 --target SmatchetTests
ctest --preset ninja-test-msys2 --output-on-failure
```

---

## Phase 2 — Callstack + P4 blame parsing doctests

**Goal**: cover every regex / parse path in callstack + blame parsing.

**Branch**: `feat/test-phase-2-callstack-p4-blame`. May run in parallel with Phase 1 (disjoint files).

### Files

| New test file | Covers | Key cases |
|---|---|---|
| `CallstackParser.test.cpp` | `Source_Core/src/CallstackParser.cpp` | `TryExtractUnrealOrModuleFunctionPrefix` · **`TryParsePathLinePair` [high-risk]** — table-driven across all 4 patterns · `ApplyPathRemaps` (longest-prefix-match, overlapping rules) · `FrameMatchesIgnoreKeywords` · `ParseCallstackText` |
| `PathRemaps.test.cpp` | `Source_Core/src/PathRemaps.cpp` | rule load, longest-prefix selection, edge cases |
| `P4BlameParse.test.cpp` | parse-only subset of `Source_Core/src/P4Blame.cpp` | `p4 annotate` output → rows; `p4 describe` envelope; empty / malformed input |

### Local gate

```bash
ctest --preset ninja-test-msys2 -R "CallstackParser|PathRemaps|P4BlameParse"
```

---

## Phase 3 — HTTP mock + in-memory SQLite fixture (+ first cache/queue tests)

**Goal**: ship the two infra primitives that unlock all remaining cache / queue / pipeline runtime testing, plus the first wave of consuming tests.

**Branch**: `feat/test-phase-3-http-sqlite-fixtures`.

### Infra files (new)

| File | Purpose |
|---|---|
| `tests/support/FakeTrackerHttpClient.h` | Implements `ITrackerHttpClient`. Scripted responses: `OnGet("/rest/api/3/issue/X").Return(json{...})`, `OnPut(...).Fail(500)`. Records calls for assertion. |
| `tests/support/FakeTrackerClient.h` | Implements `ITrackerClient`. Bypasses HTTP entirely. |
| `tests/support/SqliteMemFixture.h` | RAII `:memory:` SQLite via `SQLiteCpp`; runs `LocalCacheManager`'s init path so tests start at known schema version. |

**Interface handling**: if `TrackerHttpClient` lacks an extracted interface, `tracker-backend` agent promotes it in the same PR — **provided** the diff stays under 200 LoC and the DX12 build (`SmatchetCore_DX12`) still compiles. If either guard fails, ship Phase 3 against the existing concrete class via a thin shim and defer full extraction to a follow-up PR (backlog category `infra`).

### Test files (new)

| Test file | Covers | High-risk cases |
|---|---|---|
| `tests/Source_Core/LocalCacheManager.test.cpp` | `Source_Core/src/LocalCacheManager.cpp` | `SaveTicket` transactional · prepared-stmt reuse · schema-version migration paths · concurrent-read safety smoke |
| `tests/Source_Core/OfflineQueueServiceRuntime.test.cpp` | `Source_Core/src/OfflineQueueService.cpp` runtime | enqueue create-issue / field-edit · drain with mocked tracker 200 / 4xx / 5xx / timeout · dead-letter promotion · audit-trail row emitted per attempt · chained pending-create → field-edit replay |
| `tests/Source_Core/TicketSyncService.test.cpp` | `Source_Core/src/TicketSyncService.cpp` | `ApplyIssueFetchPack` partial-fetch (no delete) vs full-sync (delete-stale) — must not wipe on partial · `TickStreamingApply` state machine · worker-result drain |
| `tests/Source_Core/BackendAuditTrail.test.cpp` | `Source_Core/src/BackendAuditTrail.cpp` | append, query-by-ticket, retention |
| `tests/Source_Core/IssueCreatePipelineIntegration.test.cpp` | end-to-end with `FakeTrackerHttpClient` | full create → cache merge → audit trail · update existing → `MergeDraftIntoCachedTicketForUpdate` · 5xx retry · 4xx dead-letter routing |

### Hostile-fixture cases

- mid-transaction throw inside `SaveTicket` → DB stays consistent.
- `ApplyIssueFetchPack` empty fetch in full-sync mode — must reject, not delete.
- queue replay with 401 → entry stays in queue, no dead-letter on auth failure.

### Local gate

```bash
ctest --preset ninja-test-msys2 -R "LocalCacheManager|OfflineQueueServiceRuntime|TicketSyncService|IssueCreatePipelineIntegration"
```

---

## Phase 4 — Config + schema migration

**Goal**: every config schema-version migration has a forward-compat test; hostile config files do not crash.

**Branch**: `feat/test-phase-4-config-migration`.

### Files

| Test file | Covers | Key cases |
|---|---|---|
| `tests/Source_Core/ConfigManager.test.cpp` | `Source_Core/src/ConfigManager.cpp` | `NormalizeDirectoryPath` (Win/POSIX) · `EnsureDirectoryExists` (success + permission-denied where testable) · `SMATCHET_USER_DATA` precedence |
| `tests/Source_Core/ConfigMigration.test.cpp` | every `config_migrate_v*` helper | per version: load fixture v_old → migrate → equals v_new · idempotent re-migrate · unknown version → clear error · truncated file → no crash · garbage JSON → default config + `LOG_ERROR` |
| `tests/fixtures/config/v{N}.json` | per-version snapshot fixtures | current shipped version + scaffold for next bump |
| `scripts/dev/test-config-migration.sh` | shell-level smoke | load each fixture via `SmatchetStandalone.exe cmd config.dump --json`; assert shape |

### Local gate

```bash
ctest --preset ninja-test-msys2 -R "ConfigManager|ConfigMigration"
bash scripts/dev/test-config-migration.sh
```

---

## Phase 5 — MCP JSON-RPC pure-logic harness

**Goal**: every byte of the JSON-RPC wire protocol has a unit-level regression gate before reaching socket I/O.

**Branch**: `feat/test-phase-5-mcp-json-rpc`.

### Infra

| File | Purpose |
|---|---|
| `tests/support/McpJsonRpcFixture.h` | Feed wire bytes to MCP request parser; inspect envelope. No socket. |

### Test files

| Test file | Covers | Key cases |
|---|---|---|
| `tests/Plugins/Mcp/McpRequestParser.test.cpp` | `Plugins/Mcp/<request parser>.cpp` | valid request → method + params · missing `jsonrpc` field rejected · `id` may be string / int / null · batch requests if supported |
| `tests/Plugins/Mcp/McpEnvelope.test.cpp` | response envelope builder | success shape · error envelope: -32700, -32601, -32602, -32603 · `id` echo |
| `tests/Plugins/Mcp/McpToolSchemas.test.cpp` | declared tool schemas | every registered tool has valid `inputSchema` · required fields documented · example payloads validate against schema |
| `tests/Plugins/Mcp/McpDispatch.test.cpp` | wire-to-command-registry bridge | known method → `CommandRegistry::Execute` · unknown method → -32601 · execution failure → -32000 + readable message |

Loopback-over-socket coverage deferred to a follow-up scenario PR (backlog category `feature`).

### Local gate

```bash
ctest --preset ninja-test-msys2 -R "Mcp"
```

---

## Phase 6 — Lua full binding round-trip rig

**Goal**: every binding in `AppController_LuaBindings.cpp` round-trips against the real `lua_State` + sol2. Sandbox + timeout invariants asserted.

**Branch**: `feat/test-phase-6-lua-bindings`.

### Infra

New CMake target `SmatchetLuaTests` (separate from `SmatchetTests` — links `lua` + `sol2`) gated by `SMATCHET_BUILD_LUA_TESTS=ON`.

| File | Purpose |
|---|---|
| `tests/Lua/CMakeLists.txt` | target wiring |
| `tests/Lua/lua_main.cpp` | doctest entrypoint |
| `tests/support/LuaHostFixture.h` | minimal `LuaAutomationHost` — real `lua_State`, sandbox, timeout; no UI |

### Test files

| Test file | Covers | Key cases |
|---|---|---|
| `tests/Lua/LuaBindings.test.cpp` | every binding in `AppController_LuaBindings.cpp` | call with valid arg → expected return · wrong arg type → typed error · too few / too many args → typed error |
| `tests/Lua/LuaSandbox.test.cpp` | sandbox allow-list | `io.open` blocked · `os.execute` blocked · `package.loadlib` blocked · `require` restricted · allow-list drift check |
| `tests/Lua/LuaTimeout.test.cpp` | timeout-budget math | infinite loop killed within budget · cooperative cancellation honored · timeout exception captured + logged · no `lua_State` leak |
| `tests/Lua/LuaStubsCompile.test.cpp` | stubs-on path | with `SMATCHET_WITH_LUA_AUTOMATION=OFF`, `AppController_LuaStubs.cpp` exposes identical-shaped symbols (compile-time include check) |

### Local gate

```bash
cmake --build --preset ninja-test-msys2 --target SmatchetLuaTests
ctest --preset ninja-test-msys2 -R "Lua"
```

---

## Phase 7 — Screenshot diff (bucket C)

**Goal**: golden-image regression gate for the rendered UI. Covers theme drift, dock-gap leaks (pink-clear sentinel), pillar-1 budgets.

**Branch**: `feat/test-phase-7-screenshot-diff`.

### Infra

| File | Purpose |
|---|---|
| New CLI command `debug.window.screenshot --outPath=<png>` in `Source_Core/src/Commands/BuiltinCommands.cpp` | dumps current swapchain to PNG (`stb_image_write` — add as FetchContent if absent) |
| `tests/support/GoldenImage.h` | PNG load + per-pixel L∞ diff + tolerance · pink-pixel scan helper |
| `tests/golden/` | committed reference images per scenario × theme |
| `scripts/dev/test-screenshot-diff.sh` | runs scenarios with screenshot hook, diffs vs golden, exits non-zero on mismatch |

### Scenarios

- Extend `priority-grid-scroll`: screenshot at frame 60, default + Norton themes.
- New scenario `dock-gap-sentinel`: clear color magenta; assert pink-pixel count = 0.
- New scenario `command-palette-fuzzy`: open palette, type "view", screenshot results.

Tolerance per-channel L∞ ≤ 4 on RGB. Goldens regenerated by `--update-golden` only — never silent overwrite. Diff PNGs written next to failed cases.

**CI gate is advisory** until two consecutive weeks of green runs (auto-promote via backlog entry).

### Local gate

```bash
bash scripts/dev/test-screenshot-diff.sh
```

---

## Phase 8 — Unreal headless smoke

**Goal**: catch header-pollution, packaging, and runtime regressions in `SmatchetCore_DX12` + `SmatchetImGuiPlugin` before they ship into a UE host.

**Branch**: `feat/test-phase-8-unreal-smoke`.

**Pre-flight**: `unreal-bridge` agent verifies a reachable UE 5.4 install. If absent, **defer entire phase** via backlog entry (`infra`, blocker: "no UE install on build env"); proceed to Phase 9 without halting.

### Infra

| File | Purpose |
|---|---|
| `tests/Unreal/SmatchetSmokeProject/` | minimal UE 5.x project; .uproject + Source/SmatchetSmoke.{Build.cs, Target.cs, .h, .cpp} |
| `tests/Unreal/SmatchetSmokeProject/Plugins/SmatchetImGuiPlugin/` | junction → `UnrealPlugins/SmatchetImGuiPlugin/` |
| `tests/Unreal/SmatchetSmokeProject/Source/Tests/Smatchet_Embedded.spec.cpp` | UE Automation Spec — `Smatchet.Embedded.Smoke`. Launches plugin, asserts ImGui context exists, runs one bound command, asserts result. |
| `scripts/dev/test-unreal-smoke.sh` | locates `UnrealEditor-Cmd.exe`, runs `-ExecCmds="Automation RunTests Smatchet.Embedded.Smoke; Quit"`, parses results JSON |
| `.github/workflows/unreal-smoke.yml` | matrix job; self-hosted runner with UE installed. Advisory only. |

CI gate **advisory** until two consecutive weeks green. Build target stays `EXCLUDE_FROM_ALL`; only the smoke job enables it.

### Local gate (when UE reachable)

```bash
bash scripts/dev/test-unreal-smoke.sh
```

---

## Phase 9 — Coverage gates + enforcement

**Goal**: automated visibility into what is being exercised. Enforce that PRs touching `Source_Core/` cannot ship without test deltas.

**Branch**: `feat/test-phase-9-coverage-gates`.

### Infra

| File | Purpose |
|---|---|
| `scripts/dev/coverage.sh` | wraps `OpenCppCoverage` (Windows default). Runs `SmatchetTests` + `SmatchetLuaTests` + screenshot scenarios under coverage; emits `coverage/index.html` + `coverage/lcov.info`. Auto-fallback to `lcov + gcov` if OpenCppCoverage fails init. |
| `.github/workflows/coverage.yml` | per-PR coverage; uploads HTML artifact; posts summary comment (covered / uncovered / delta vs base) |
| `.github/workflows/coverage-gate.yml` | per-PR check: if diff touches `Source_Core/` and `tests/` shows zero net new `TEST_CASE` / `CHECK`, fail with clear message. Override label `tests-out-of-band`. |
| `docs/backlog/AGENT_SELF_IMPROVEMENT.md` entry | documents gate + override path |

### Acceptance

- Coverage HTML available per PR.
- Coverage delta posted in PR comment.
- `Source_Core/` change without test delta → blocked unless override label.

---

## Cross-phase verification

```bash
# Doctest rig — Phases 1-6
cmake --build --preset ninja-test-msys2 --target SmatchetTests SmatchetLuaTests
ctest --preset ninja-test-msys2 --output-on-failure

# Dev scripts — Phases 4, 7
bash scripts/dev/test-all.sh

# UE smoke — Phase 8 (separate runner; advisory)
bash scripts/dev/test-unreal-smoke.sh

# Coverage — Phase 9
bash scripts/dev/coverage.sh
```

End-state targets:
- **≥ 70 % line coverage** on `Source_Core/src/` excluding ImGui/UI files.
- **≥ 90 %** on the high-risk units listed in Context.
- UE smoke green two consecutive weeks → flip from advisory to blocking.
- Coverage-gate active and blocking on `Source_Core/` diffs without test deltas.

---

## Critical files referenced

Production code (read-only here; tests target these):

- `Source_Core/src/IssueCreatePipeline.cpp:49-298`
- `Source_Core/src/IssueDraft.cpp:79-375`
- `Source_Core/src/TrackerFieldValueParser.cpp:15-897`
- `Source_Core/src/TrackerFieldValueUtils.cpp`
- `Source_Core/src/TrackerFieldValuePayload.cpp`
- `Source_Core/src/TrackerLabelsEditor.cpp`
- `Source_Core/src/TrackerDateTimeFieldEditor.cpp`
- `Source_Core/src/TrackerFieldCatalog.cpp`
- `Source_Core/src/JqlSurgery.cpp`
- `Source_Core/src/CallstackParser.cpp:19-197`
- `Source_Core/src/PathRemaps.cpp`
- `Source_Core/src/P4Blame.cpp`
- `Source_Core/src/LocalCacheManager.cpp:117-150+`
- `Source_Core/src/OfflineQueueService.cpp:28-54+`
- `Source_Core/src/TicketSyncService.cpp:44-150+`
- `Source_Core/src/BackendAuditTrail.cpp`
- `Source_Core/src/FieldEditAuditSource.cpp`
- `Source_Core/src/ConfigManager.cpp:74-114+`
- `Source_Core/src/Commands/CommandRegistry.cpp:19-38`
- `Source_Core/src/Commands/BuiltinCommands.cpp`
- `Source_Core/src/AppController_LuaBindings.cpp`
- `Source_Core/src/AppController_LuaStubs.cpp`
- `Plugins/Mcp/` (all `.cpp`/`.h`)
- `Plugins/LuaConsole/` (all `.cpp`/`.h`)
- `UnrealPlugins/SmatchetImGuiPlugin/`

Rig + infra (new / edited):

- `tests/CMakeLists.txt` — extend
- `tests/Source_Core/*.test.cpp` — many new files (Phases 1-4)
- `tests/Plugins/Mcp/*.test.cpp` — new (Phase 5)
- `tests/Lua/*` — new dir + target (Phase 6)
- `tests/Unreal/SmatchetSmokeProject/` — new (Phase 8)
- `tests/support/{FakeTrackerHttpClient,FakeTrackerClient,SqliteMemFixture,McpJsonRpcFixture,LuaHostFixture,GoldenImage}.h` — new
- `tests/golden/*.png` — new (Phase 7)
- `tests/fixtures/config/v{N}.json` — new (Phase 4)
- `scripts/dev/test-{config-migration,screenshot-diff,unreal-smoke}.sh` — new
- `scripts/dev/coverage.sh` — new (Phase 9)
- `.github/workflows/{unreal-smoke,coverage,coverage-gate}.yml` — new (Phases 8-9)
- `CMakePresets.json` — possibly extend (`ninja-coverage-msys2` if needed)

---

## Implementation log

(Append per shipped PR — `<sha> · <one-line summary>`.)

- Phase 3 — 2026-05-16 · `feat/test-phase-3-http-sqlite-fixtures`
  - `tests/support/FakeTrackerClient.h` — scripted in-memory `ITrackerClient` implementation. Records `CreateIssue`, `UpdateIssueFields`, `UpdateField`, `AddIssueToSprint`, `AttachFilesToIssue` call sequences. Per-method scripted-reply queues (`EnqueueCreateIssueSuccess(key)` / `EnqueueCreateIssueFailure(err)`) with sticky-default fallback. Header-only.
  - `tests/support/SqliteMemFixture.h` — RAII `:memory:` `LocalCacheManager`. Reopen helper for idempotent-init regression. Header-only.
  - `tests/Source_Core/LocalCacheManager.test.cpp` — 27 cases / 113 assertions covering SaveTicket (round-trip, full-snapshot replace, rich-value parallel table, empty-rich skip), prepared-statement reuse (25-iter loop), DeleteTicket, GetAllTickets, ForEachTicket, cache_meta flag, pending_creates lifecycle (enqueue/load/update/payload-update/archive/restore/delete/legacy-drop), pending_field_edits lifecycle (enqueue with rich value, mark+resolve conflict, attempts-bump, archive, delete-active, delete-dead), and idempotent re-init.
  - `tests/Source_Core/BackendAuditTrail.test.cpp` — 9 cases / 33 assertions covering MakeOperationId uniqueness + prefix fallback, RedactText sensitive-key matrix + 1000-char truncation, RedactJson recursive-object + field-diff redaction by field_id, MakeFieldDiffUnknownBefore array-vs-non-object, AppendBegin/AppendResult round-trip via ReadRecentEvents (filtered by operation_id), failure error-string passthrough.
  - `tests/Source_Core/IssueCreatePipelineIntegration.test.cpp` — 11 cases / 49 assertions exercising `IssueCreatePipeline::Run` end-to-end with `FakeTrackerClient` + `:memory:` cache. Covers: create-success seeds cache, null-cache fallback, missing-required-field validation, CreateIssue-failure propagation, BuildCreatePayload-failure propagation, update path via ExistingIssueKey, MergeDraftIntoCachedTicketForUpdate real-path (preserves untouched cache fields), UpdateIssueFields-failure leaves cache untouched, empty PUT payload skips network, attachment failures pass through, legacy `FieldValues["key"]` update dispatch, whitespace-only `key` falls through to create.
  - `tests/CMakeLists.txt` — registered 3 new test files, added `LocalCacheManager.cpp`, `BackendAuditTrail.cpp`, `IssueCreatePipeline.cpp`, `TrackerFieldPayload.cpp`, `CompactDateFormat.cpp`, `UiPerfMonitor.cpp`, `MarkdownConvert.cpp` to the per-target source list. Linked `md4c` at runtime (for `MarkdownConvert`). Added `tests/support` to `target_include_directories`.
  - Test totals after Phase 3: 190 cases (up from 143) / 755 assertions (up from 530). 0 failed.

- Phase 2 — 2026-05-16 · `feat/test-phase-2-callstack-p4-blame`
  - `tests/Source_Core/CallstackParser.test.cpp` — 7 cases / 54 assertions covering `ParseCallstackText` across MSVC `path(line)`, Unreal `Module!Func [path(line)]`, Clang `path:line[:col]`, GDB `at FUNC PATH:LINE`; `ApplyPathRemaps` (single rule, no-match, empty list, longest-prefix wins, longest-prefix wins regardless of declaration order, empty `FromPrefix` ignored, equal-prefix-tie broken by last-rule index, full-path prefix, prefix-longer-than-path); `FrameMatchesIgnoreKeywords` (empty list, empty keyword, raw/function/path hits, case-insensitive, multi-keyword OR).
  - `tests/CMakeLists.txt` — registered `CallstackParser.test.cpp` and added `Source_Core/src/CallstackParser.cpp` to the per-target source list. No new link libs (CallstackParser.h pulls only `ConfigManager.h` for `PathRemapRule`, already covered by existing linkage).
  - Test totals after Phase 2: 143 cases (up from 136) / 530 assertions (up from 476). 0 failed.

- Phase 1 — 2026-05-16 · `feat/test-phase-1-tracker-pure-logic`
  - `tests/Source_Core/IssueDraft.test.cpp` — 26 cases / 80 assertions covering Resolve/From/Missing/ToJson/FromJson/ComputeFieldChanges/PruneUnchangedFields/MapFieldIdsToNames + suppressed/special id predicates + default inherit list.
  - `tests/Source_Core/IssueCreatePipeline.test.cpp` — 9 cases / 19 assertions on `MergeDraftIntoCachedTicketForUpdate` (extracted to a new pure helper TU; see "Deviations").
  - `tests/Source_Core/TrackerFieldValueParser.extended.test.cpp` — 32 cases / 91 assertions covering Build/From/Merge option helpers + ClassifyTrackerFieldFamily + NormalizeTrackerFieldValue + ParseComments + ParseChangelog + Format/Json scalar helpers + user-sort + array append.
  - `tests/Source_Core/TrackerFieldValueUtils.test.cpp` — 16 cases / 47 assertions covering FindOptionRecursive (incl. recursive descent), ResolveOptionId/Label, ResolveCurrentSelectionIds, EmptySelectPreviewLabel, TryResolveCascadingSelection (all four input shapes), and the three time-duration field predicates.
  - `Source_Core/include/IssueCreatePipelineHelpers.h` + `Source_Core/src/IssueCreatePipelineHelpers.cpp` — new pure-helper TU exposing `MergeDraftIntoCachedTicketForUpdate` outside of `IssueCreatePipeline.cpp`'s anonymous namespace. Picked up automatically by `Source_Core/src/*.cpp` GLOB in the main `CMakeLists.txt`.
  - `tests/CMakeLists.txt` — registered 4 new `.test.cpp` files, added `IssueDraft.cpp` + `IssueCreatePipelineHelpers.cpp` to the per-target source list, and linked `SQLiteCpp` for headers only (LocalCacheManager.h pulls `<SQLiteCpp/SQLiteCpp.h>` transitively via `IssueDraft.h`; no test calls into SQLite).

## Deviations from plan

(Append per shipped PR — what changed vs this plan + one-line rationale.)

- Phase 3 — `tests/support/FakeTrackerHttpClient.h` not shipped. `TrackerHttpClient` is already free-function-style with an injectable `requestFn` lambda (see `Source_Core/include/TrackerHttpClient.h:50-53`); no interface extraction needed. The plan's "promote `ITrackerHttpClient`" path is therefore not applicable. All Phase 3 HTTP simulation runs through the higher-level `FakeTrackerClient` (which implements `ITrackerClient` directly — the natural mock layer for `IssueCreatePipeline::Run`).
- Phase 3 — `tests/Source_Core/OfflineQueueServiceRuntime.test.cpp` + `tests/Source_Core/TicketSyncService.test.cpp` deferred. Both services hold `AppController&` back-references and reach into 8-10 different AppController-private fields each (`Cache`, `Backend`, `ActiveTickets`, `activeTicketsMutex_`, `lastTrackerConnectivityState_`, several callback fields). Standing up a real `AppController` for tests is impractical (the class is 1000-line header + 1700-line impl with ImGui / GLFW / OpenGL touch points). Their own header migration comments call this out: `OfflineQueueService.h:13-17` says Phase 2 of the AppController-detangle migration will "introduce small interface bundles so this service no longer needs `friend` access". That interface-bundle work is the prerequisite for runtime tests. Backlog entry `2026-05-16 · offline-sync · [infra] — Phase 3 OfflineQueueServiceRuntime + TicketSyncService deferred` tracks the follow-up. Phase 3 hostile-fixture cases ("queue replay with 401 stays in queue", "ApplyIssueFetchPack empty fetch must not delete") move with that follow-up — neither is reachable today.
- Phase 3 — `BackendAuditTrail.test.cpp` does NOT include a "missing audit file returns empty" case. The async writer is a process-wide singleton; once the first audit event in the test binary runs, the writer thread captures the audit file path for the rest of the process. Subsequent tests that change `ConfigManager::SetUserDataDirectory` can't redirect the writer, so a "no audit file" assertion is unreliable. Backlog entry `2026-05-16 · offline-sync · [test] — Phase 3 BackendAuditTrail async-writer process-wide singleton` proposes moving the path capture inside the loop body in production code. Workaround in test: scope IO-surface assertions to one TEST_CASE body with shared `AuditDirGuard` + operation_id filtering.
- Phase 3 — `LocalCacheManager.test.cpp` mid-transaction-throw hostile-fixture is NOT shipped. The test exists implicitly: SQLite's RAII `SQLite::Transaction` rolls back on stack unwind, and `SaveTicket` already wraps its DB ops in a transaction that re-throws on inner exceptions (`LocalCacheManager.cpp:130-175`). A test that simulates a throw between INSERTs would need either a SQLite-level fault-injection layer (out of scope) or a custom `ITransaction` interface (production refactor not in Phase 3 scope). Documented as an open hostile-fixture for the follow-up runtime PR.

- Phase 2 — `PathRemaps.test.cpp` not added as a standalone file: the plan named `Source_Core/src/PathRemaps.cpp` as a separate TU, but no such file exists. `ApplyPathRemaps` lives inside `Source_Core/src/CallstackParser.cpp`; coverage was folded into `tests/Source_Core/CallstackParser.test.cpp` (9 SUBCASEs, including the documented mutation-sanity case for longest-prefix selection).
- Phase 2 — `P4BlameParse.test.cpp` deferred: every parsing helper named in the plan (`ParseAnnotateTextLine`, `ParseLatestChangeFromChangesOutput`, `SplitLines`, `StripP4UserDomain`) is `static`-in-anonymous-namespace inside `Source_Core/src/P4Blame.cpp`. The public surface (`P4BlameLine`, `P4AnnotateFile`) spawns the `p4` process, which is out of scope for the pure-logic doctest rig. Phase 2 write set is tests-only per the orchestrator packet, so the TU split needed to expose the parsers (same pattern as `IssueCreatePipelineHelpers` in Phase 1) lands as a separate prep PR. Backlog entry `2026-05-16 · test-rig · [infra] — Phase 2 P4BlameParse.test.cpp deferred` tracks the follow-up plan (split + tests, ≈1 h combined).
- Phase 2 — `TryParsePathLinePair` and `TryExtractUnrealOrModuleFunctionPrefix` are exercised through the public `ParseCallstackText` rather than direct calls (both live in anonymous namespace inside `CallstackParser.cpp`). The MSVC-line-number mutation-sanity target is satisfied: flipping `outLine = std::stoi(m[2].str())` to `+ 1` produced 5 assertion failures in the MSVC subcases (revert verified green).

- `TrackerLabelsEditor.test.cpp` / `TrackerDateTimeFieldEditor.test.cpp` deferred — both `.cpp` files include `<imgui.h>` + `AppController.h`; the only purely-public helpers are single-line `Is*Field` predicates and don't justify a test target. Future fix is to extract the pure ParseCsv / SortAndUniqueLabels / Parse*Date* helpers into a separate TU.
- `TrackerFieldValuePayload.test.cpp` deferred — the file named in the plan (`Source_Core/src/TrackerFieldValuePayload.cpp`) does not exist in the tree; the closest match (`TrackerFieldPayload.cpp`) transitively includes `JiraClient.h` (cpr + ConfigManager + full HTTP surface) via `TrackerFieldPayload.h`. Not in scope for this phase.
- `JqlSurgery.test.cpp` deferred — no `JqlSurgery.cpp` exists; the JQL surface is already covered by the existing `tests/Source_Core/JqlProjectScope.test.cpp`. Plan name aliased an older path.
- `TrackerFieldCatalog.test.cpp` deferred — `TrackerFieldCatalog.cpp` `#include "JiraClient.h"`, which pulls in cpr / ConfigManager / ITrackerClient. Pure parts would need to be extracted into a separate TU first.
- `IssueCreatePipeline.test.cpp` scope trimmed — `MergeDraftIntoCachedTicketForUpdate` was moved to a new `Source_Core/include/IssueCreatePipelineHelpers.h` + dedicated `.cpp` (clean refactor, no public-header touch on `IssueCreatePipeline.h`). `ApplyPostIssueSteps` decision logic is **deferred**: the helper drives `ITrackerClient::UpdateIssueFields` / `AddIssueToSprint` / `AttachFilesToIssue`, so unit-testing its branches requires a mock client. That work belongs alongside an `ITrackerClient` mock harness (Phase 3+ HTTP layer).

## Verification

(Append per shipped PR — what ran + result.)

- Phase 3 — 2026-05-16
  - `cmake --build --preset ninja-test-msys2 --target SmatchetTests` — clean.
  - `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone` — clean (new production-side code paths still link; no new production code was added).
  - `ctest --output-on-failure` from `build/ninja-test-msys2`: `1/1 smatchet_tests Passed (0.18 sec)`.
  - `SmatchetTests.exe`: 190 test cases (up from 143) / 755 assertions (up from 530), 0 failed, 0 skipped.
  - Mutation sanity (production-side, 4 cases):
    - `LocalCacheManager.cpp::SaveTicket` — commented out the `DELETE FROM ticket_field_values WHERE ticket_id = ?` step. `LocalCacheManager: SaveTicket replaces the full field-value snapshot (delete+insert under one tx) [high-risk]` failed at lines 68/69 with `priority` / `labels` still 1. Reverted to green.
    - `LocalCacheManager.cpp::ArchivePendingCreate` — inverted `terminalError` precedence (always read `last_error` from row). `LocalCacheManager: ArchivePendingCreate moves row to dead-letter with metadata [high-risk]` failed at line 221 (`tracker error` ≠ `final tracker error`). Reverted to green.
    - `LocalCacheManager.cpp::MarkFieldEditConflict` — flipped `has_merge_conflict = 1` to `= 0`. `LocalCacheManager: MarkFieldEditConflict + ResolveFieldEditConflict flow [high-risk]` failed at line 328 (`HasMergeConflict` false). Reverted to green.
    - `IssueCreatePipeline.cpp::Run` — short-circuited the `ExistingIssueKey` dispatch so update path is never taken. `IssueCreatePipeline: Run dispatches to update path when ExistingIssueKey is set` failed at line 169 (`BuildUpdatePayloadCallCount` == 0, expected 1) and line 175 (cache row still `Original`). Reverted to green.

- Phase 2 — 2026-05-16
  - `cmake --build --preset ninja-test-msys2 --target SmatchetTests` — clean.
  - `ctest --output-on-failure` from `build/ninja-test-msys2`: `1/1 smatchet_tests Passed (0.04 sec)`.
  - `SmatchetTests.exe`: 143 test cases (up from 136) / 530 assertions (up from 476), 0 failed, 0 skipped.
  - Mutation sanity (production-side, on `Source_Core/src/CallstackParser.cpp`):
    - `ApplyPathRemaps` longest-prefix select (`from.size() >= bestLen` → `<= bestLen`): 8 assertion failures across the longest-prefix SUBCASEs (`longest-prefix wins`, `longest-prefix wins regardless of rule order`, `later rule with equal-length prefix wins`). Reverted to green.
    - `TryParsePathLinePair` MSVC line-number capture (`outLine = std::stoi(m[2].str())` → `+ 1`): 5 assertion failures across the MSVC SUBCASEs (line 1 sentinel, line 123, large line 987654, bare-relative line 7, Unreal `Module!Class::Method() [path(line)]` line 42). Reverted to green.

- Phase 1 — 2026-05-16
  - `cmake --build --preset ninja-test-msys2 --target SmatchetTests` — clean.
  - `cmake --build --preset ninja-debug-msys2 --target SmatchetStandalone` — clean (new helper TU compiles into the production exe via `Source_Core/src/*.cpp` glob).
  - `ctest --output-on-failure` from `build/ninja-test-msys2`: `1/1 smatchet_tests Passed (0.03 sec)`.
  - `SmatchetTests.exe`: 136 test cases (up from 73) / 476 assertions (up from 305), 0 failed, 0 skipped.
  - Mutation sanity (5 high-risk cases): one production-side mutation per case, verified each fails the target test, reverted afterward.
    - `PruneUnchangedFields_DoesNotEraseOnCacheMiss`: force-erase on cache-miss → 1 failure (target case).
    - `ToJsonFromJson_RoundTripPreservesNumericTypes`: drop integer coercion in `FromJson` → 1 failure (target case).
    - `MergeDraftIntoCachedTicketForUpdate_HonoursPutFieldsSucceeded`: overlay all draft fields ignoring `putFieldsSucceeded` → 2 failures (target case + neighbour `skips keys absent from draft`).
    - `MergeTrackerFieldOption_DoesNotDuplicateById`: invert dedup-key check → 4 failures (target case + 3 dedup-by-children/value/disabled siblings).
    - `ClassifyTrackerFieldFamily_DetectsCascadingSelect`: force `hasChildren=false` → 1 failure (target case).

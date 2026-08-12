# Test suite expansion — completion plan (Phases 4-9 + carry-overs)

> Continuation of [`docs/plans/shipped/test-suite-expansion.md`](archive/test-suite-expansion.md). Phases 1-3 shipped 2026-05-16 ([#103](https://github.com/alexandrosk0/Smatchet/pull/103), [#106](https://github.com/alexandrosk0/Smatchet/pull/106), [#107](https://github.com/alexandrosk0/Smatchet/pull/107)). This plan sequences the remaining work end-to-end with locked decisions, parallelisation rules, and per-slice agent packets.

## Locked decisions (2026-05-16 user review)

| Decision | Value | Notes |
|---|---|---|
| Phase 8 — UE 5.4 smoke | **Defer** | No reachable UE install on build env. Backlog entry `infra` / blocker "no UE install". Phase 9 lands without Phase 8 gates. Re-evaluate when UE install lands. |
| `large-files-and-phase-2` Track B | **Resume early** — after PR D (`offline-queue-deps-interface`) lands | Original gate was "Phase 9 ships". PR D removes the largest Track B conflict (`OfflineQueueService.cpp` + `AppController.h`); Track B may resume against narrower-than-umbrella write set after D. |
| Coverage tool (Phase 9) | **`OpenCppCoverage` Windows-only**, `lcov+gcov` documented fallback | No POSIX runner today. Acceptable per user. Document POSIX gap in `scripts/dev/coverage.sh` header + Phase 9 PR body. |
| Carry-overs | **Ship all first**, then Phase 4-9 sequential | Eliminates concurrent-PR conflict risk during forward phases. Saves rebase cost on Phase 4-9 vs interleaved alternative. |

Plus existing locked defaults from [§ Locked defaults](archive/test-suite-expansion.md#locked-defaults--no-clarifications-needed) of the parent plan (still in force): `TrackerHttpClient` interface shim, GL-context CI policy, mutation testing out of scope, `IScenario` reuse, ImGui Test Engine extension, blocking-vs-non-blocking deferral protocol.

## Carry-over inventory

From Phase 1-3 deferrals plus backlog polish entries:

| Slug | Source | Cost | Prereq | Agent | Branch |
|---|---|---|---|---|---|
| `callstack-adversarial-subcases` | Phase 2 polish (backlog 2026-05-16 `code-review+security-review`) | 30 min | none | `test-rig` | `feat/test-callstack-adversarial` |
| `p4blame-parse-tu-split` | Phase 2 deferral (backlog 2026-05-16 `test-rig · [infra]` P4) | 1 h | none | `test-rig` (TU-split pre-authorised in packet) | `feat/p4blame-parse-tu-split` |
| `tracker-labels-pure-tu` | Phase 1 deferral | 1 h | none | `test-rig` | `feat/tracker-labels-pure-tu` |
| `tracker-datetime-pure-tu` | Phase 1 deferral | 1 h | none | `test-rig` | `feat/tracker-datetime-pure-tu` |
| `tracker-payload-pure-tu` | Phase 1 deferral | 1 h | none | `test-rig` | `feat/tracker-payload-pure-tu` |
| `tracker-field-catalog-pure-tu` | Phase 1 deferral | 1 h | none | `test-rig` | `feat/tracker-field-catalog-pure-tu` |
| `offline-queue-deps-interface` (PR D — `IOfflineQueueDeps` + `ITicketSyncDeps` bundles + `AppControllerDepsAdapter`) | Phase 3 deferral (backlog 2026-05-16 `offline-sync · [infra]`) | 1-2 d | none | `offline-sync` | `feat/offline-queue-deps-interface` |
| `offline-queue-runtime-tests` (PR E) | Phase 3 deferral | ~2 h | PR D shipped | `test-rig` | `feat/offline-queue-runtime-tests` |
| `ticket-sync-service-tests` (PR F) | Phase 3 deferral | ~2 h | PR D shipped | `test-rig` | `feat/ticket-sync-service-tests` |

Total carry-over: ~12-16 h excluding PR D (1-2 d). PR D dominates wall-clock.

## Forward-phase inventory (Phases 4-9)

| Phase | Topic | Cost | New infra? | Agent dispatch |
|---|---|---|---|---|
| 4 | Config + schema migration | 1 d | none | `test-rig` (orchestrator handles `scripts/dev/test-config-migration.sh`) |
| 5 | MCP JSON-RPC pure-logic harness | 1 d | `tests/support/McpJsonRpcFixture.h` | `test-rig` + `mcp-toolsmith` |
| 6 | Lua bindings round-trip rig | 1.5 d | new `SmatchetLuaTests` target | `test-rig` + `lua-binder` |
| 7 | Screenshot diff (bucket C) | 1 d | `debug.window.screenshot` CLI + `tests/support/GoldenImage.h` + `tests/golden/` | `test-author` + `command-system` |
| ~~8~~ | ~~Unreal headless smoke~~ | DEFERRED | — | (re-evaluate when UE install reachable) |
| 9 | Coverage gates + enforcement (Windows-only `OpenCppCoverage`) | 0.5 d | `scripts/dev/coverage.sh` + `.github/workflows/coverage{,-gate}.yml` | `build-doctor` + `test-author` |

## Execution order

Sequential by default to keep `develop` cleanly bisectable. Two parallelisation points only:

1. **Carry-over wave A** — `callstack-adversarial-subcases` + `p4blame-parse-tu-split` + four `tracker-*-pure-tu` slices share zero write-set overlap. Up to four can run in parallel on independent branches.
2. **PR E + PR F** — both consume PR D's bundle, disjoint test files. Parallel after D merges.

PR D, Phase 4, Phase 5, Phase 6, Phase 7, Phase 9 sequential because each touches `tests/CMakeLists.txt` (single line of conflict, but cheap to keep linear).

Concrete sequence:

```
day 1     Wave A (6 carry-over PRs)          parallel up to 4-wide
day 2     PR D (offline-queue-deps)          alone — heavy refactor
day 3     PR D ctd; Track B unblock (out of scope of this plan)
day 4     PR E + PR F                        parallel — disjoint test files
day 5     Phase 4 (Config + migration)
day 6     Phase 5 (MCP JSON-RPC)
day 7-8   Phase 6 (Lua bindings)
day 9     Phase 7 (Screenshot diff)
day 10    Phase 9 (Coverage gates) — Windows-only; POSIX fallback documented
```

~10 day wall-clock for completion. Phase 8 deferred to a separate future plan once a UE install lands.

## Per-slice agent packet template

Every dispatched packet must include:

1. **Plan-lock pre-flight** — *"Read `docs/plans/active/_plan-locks.md` first. Refuse if your write set overlaps an `in-flight` or `claimed` entry; surface to the orchestrator."* Orchestrator computes intersection and writes the new claim before dispatch.
2. **Write set bounds** explicit — narrow paths per slice; no umbrella claims.
3. **Test-rig `<Unit>Parse.{h,cpp}` TU-split pre-authorisation** for any unit with anon-namespace pure helpers (per AGENTS.md applied 2026-05-16 rule).
4. **Plan-time production-file existence check** (5 s Glob) before finalising the packet (per AGENTS.md applied 2026-05-16 rule).
5. **Output budget** — `Report ≤ 200 words, table form, no prose paragraphs`.
6. **Plan revision contract** — append to `docs/plans/shipped/test-suite-expansion.md` § Implementation log + § Deviations + § Verification in the same commit per AGENTS.md § Plan revision after implementation.
7. **Mutation-sanity recipe** — one production-side mutation per high-risk case, demonstrably fails the new test, reverted before commit.

## Per-slice scoping

### Carry-over A — `callstack-adversarial-subcases`

- **Write set**: `tests/Source_Core/CallstackParser.test.cpp` only.
- **Cases to add**: pin `f.Function` substring at line 88 (currently `CHECK_FALSE(empty)`); `INT_MAX+1` line number (assert zero frames); ≥64 KiB single line (assert <50 ms wall-clock); `..`-traversal path through `ApplyPathRemaps` (assert unchanged); NUL-embedded fixture.
- **Gate**: ctest `-R CallstackParser`; perf wall-clock measured inline.

### Carry-over B — `p4blame-parse-tu-split`

- **Write set**: `Source_Core/include/P4BlameParse.h` (NEW), `Source_Core/src/P4BlameParse.cpp` (NEW), `Source_Core/src/P4Blame.cpp` (call site rewire), `tests/Source_Core/P4BlameParse.test.cpp` (NEW), `tests/CMakeLists.txt`.
- **Closure rule**: lift `ParseAnnotateTextLine`, `ParseLatestChangeFromChangesOutput`, `SplitLines`, `StripP4UserDomain` out of `P4Blame.cpp`'s anonymous namespace into the new TU. `P4Blame.cpp` calls them via using-decl block.
- **Cases**: 8 cases / ~30 CHECKs — four annotate-line shapes, `Change N on DATE by USER` parse, empty/malformed/`@`-stripped users, line-split with no trailing `\n`.
- **Mutation target**: invert the `m[2]` capture in `ParseAnnotateTextLine`; expect column-shift assertions to fail.

### Carry-overs C1-C4 — `tracker-{labels,datetime,payload,field-catalog}-pure-tu`

Per-unit `*Pure.cpp` + `*Pure.h` TU split lifting pure helpers out of the ImGui/cpr/JiraClient-tainted production `.cpp`:

| Slug | Production split | Helpers lifted | Test count |
|---|---|---|---|
| `tracker-labels-pure-tu` | `Source_Core/{include,src}/TrackerLabelsPure.{h,cpp}` (NEW); `TrackerLabelsEditor.cpp` rewires | `ParseCsv`, `SortAndUniqueLabels`, dup detection, whitespace normalisation | ~6 cases / ~20 CHECKs |
| `tracker-datetime-pure-tu` | `Source_Core/{include,src}/TrackerDateTimePure.{h,cpp}` (NEW) | ISO-8601 parser, rejected formats, tz handling | ~5 cases / ~15 CHECKs |
| `tracker-payload-pure-tu` | `Source_Core/{include,src}/TrackerFieldPayloadPure.{h,cpp}` (NEW) | per-family payload builders (string/number/option/multi-option/user/cascading) free of `JiraClient.h` | ~10 cases / ~30 CHECKs |
| `tracker-field-catalog-pure-tu` | `Source_Core/{include,src}/TrackerFieldCatalogPure.{h,cpp}` (NEW) | catalog merge, field-id lookup, allowed-values filtering | ~6 cases / ~20 CHECKs |

Each PR: production `.cpp` keeps non-pure surface; pure header has zero `<imgui.h>` / `<cpr/*>` / `JiraClient.h` / `AppController.h` includes. Test file links the new pure TU only.

### PR D — `offline-queue-deps-interface`

- **Branch**: `feat/offline-queue-deps-interface`. Owner: `offline-sync`.
- **Write set**: `Source_Core/include/IOfflineQueueDeps.h` (NEW), `Source_Core/include/ITicketSyncDeps.h` (NEW), `Source_Core/include/AppControllerDepsAdapter.h` (NEW), `Source_Core/src/AppControllerDepsAdapter.cpp` (NEW), `Source_Core/include/OfflineQueueService.h`, `Source_Core/src/OfflineQueueService.cpp`, `Source_Core/include/TicketSyncService.h`, `Source_Core/src/TicketSyncService.cpp`, `Source_Core/include/AppController.h` (drop `friend` decls), `Source_Core/src/AppController.cpp` (wire adapter).
- **Interface bundles**: `IOfflineQueueDeps` exposes `Cache()`, `Backend()`, `LookupActiveTicketFieldValue()`, `ActiveTicketsMutex()`, `LastTrackerConnectivityState()`, `PushOfflineReplayTimersDuringTransportOutage()`, `RequestDeferredLiveTrackerBackendSuccessNotify()`, `WarmIssueTypeEditMetaAtStartAsync()`, `NotifyLuaTicketDataChanged()`. `ITicketSyncDeps` mirrors the `TicketSyncService` slice.
- **Adapter**: `AppControllerDepsAdapter` implements both interfaces against a real `AppController&` for production.
- **Tests**: `tests/support/FakeOfflineQueueDeps.h` + `tests/support/FakeTicketSyncDeps.h` (NEW, headers only) ship in this PR so PR E + PR F have ready fixtures.
- **No semantic change** — pure interface extraction. `code-review` + `security-review` parallel.
- **Track B handshake**: after PR D merges, update `docs/plans/active/_plan-locks.md` Track B entry — `on-hold` → `claimed` (or `in-flight` if user resumes B1 immediately). B1 (`ICacheAccess.h`) write set no longer overlaps `OfflineQueueService.h`.

### PR E — `offline-queue-runtime-tests`

- **Branch**: `feat/offline-queue-runtime-tests`. Owner: `test-rig`.
- **Write set**: `tests/Source_Core/OfflineQueueServiceRuntime.test.cpp` (NEW), `tests/CMakeLists.txt`.
- **Cases**: ~12 cases / ~50 CHECKs — enqueue create-issue / field-edit, drain with `FakeTrackerClient` 200/4xx/5xx/timeout, dead-letter promotion, audit-trail row per attempt, chained pending-create → field-edit replay, **401 stays in queue (no dead-letter)**, retry-cap boundary.

### PR F — `ticket-sync-service-tests`

- **Branch**: `feat/ticket-sync-service-tests`. Owner: `test-rig`.
- **Write set**: `tests/Source_Core/TicketSyncService.test.cpp` (NEW), `tests/CMakeLists.txt`.
- **Cases**: ~12 cases / ~50 CHECKs — `ApplyIssueFetchPack` partial-fetch (no delete) vs full-sync (delete-stale), **empty fetch in full-sync mode rejects not deletes**, `TickStreamingApply` state machine, worker-result drain.

### Phase 4 — Config + schema migration

- **Branch**: `feat/test-phase-4-config-migration`. Owner: `test-rig`.
- **Write set**: `tests/Source_Core/ConfigManager.test.cpp` (NEW), `tests/Source_Core/ConfigMigration.test.cpp` (NEW), `tests/fixtures/config/v{N}.json` (per-version fixtures), `scripts/dev/test-config-migration.sh` (NEW), `tests/CMakeLists.txt`.
- **Cases**: `NormalizeDirectoryPath` (Win/POSIX), `EnsureDirectoryExists`, `SMATCHET_USER_DATA` precedence, per-version migration load → migrate → assert v_new, idempotent re-migrate, unknown-version error, truncated file no crash, garbage JSON falls back to default + `LOG_ERROR`.
- **Shell smoke**: `scripts/dev/test-config-migration.sh` loads each fixture via `SmatchetStandalone.exe cmd config.dump --json`; auto-enrols into `scripts/dev/test-all.sh`.

### Phase 5 — MCP JSON-RPC pure-logic harness

- **Branch**: `feat/test-phase-5-mcp-json-rpc`. Owner: `test-rig` + `mcp-toolsmith`.
- **Write set**: `tests/support/McpJsonRpcFixture.h` (NEW), `tests/Plugins/Mcp/McpRequestParser.test.cpp` (NEW), `tests/Plugins/Mcp/McpEnvelope.test.cpp` (NEW), `tests/Plugins/Mcp/McpToolSchemas.test.cpp` (NEW), `tests/Plugins/Mcp/McpDispatch.test.cpp` (NEW), `tests/CMakeLists.txt`.
- **Cases**: parse + envelope + dispatch + schema integrity. Loopback-over-socket deferred per parent plan.
- **`mcp-toolsmith` scope**: confirm tool schemas + dispatch wiring stay byte-identical pre/post; no production change.

### Phase 6 — Lua bindings round-trip rig

- **Branch**: `feat/test-phase-6-lua-bindings`. Owner: `test-rig` + `lua-binder`.
- **Write set**: `tests/Lua/CMakeLists.txt` (NEW), `tests/Lua/lua_main.cpp` (NEW), `tests/support/LuaHostFixture.h` (NEW), `tests/Lua/LuaBindings.test.cpp` (NEW), `tests/Lua/LuaSandbox.test.cpp` (NEW), `tests/Lua/LuaTimeout.test.cpp` (NEW), `tests/Lua/LuaStubsCompile.test.cpp` (NEW), `CMakeLists.txt` (new `SMATCHET_BUILD_LUA_TESTS` option), `CMakePresets.json` (extend `ninja-test-msys2`).
- **Hard invariants** (per `agents/lua-binder.md`): sol2 v2.20.6 usertype member fns take plain args (no `sol::this_state` first param); Lua 5.3-as-C++ requires `luaconf.h` `extern "C"` patch.

### Phase 7 — Screenshot diff (bucket C)

- **Branch**: `feat/test-phase-7-screenshot-diff`. Owner: `test-author` + `command-system`.
- **Write set**: `Source_Core/src/Commands/BuiltinCommands.cpp` (new `debug.window.screenshot` command), `tests/support/GoldenImage.h` (NEW), `tests/golden/*.ppm` (NEW reference images), `scripts/dev/test-screenshot-diff.sh` (NEW), `Source_Core/src/Commands/Scenarios/DockGapSentinelScenario.cpp` (NEW), `Source_Core/src/Commands/Scenarios/CommandPaletteFuzzyScenario.cpp` (NEW), scenario registry, `cmake/`.
- **Tolerance**: per-channel L∞ ≤ 4 on RGB. Goldens regenerated only via `--update-golden`.
- **CI gate**: advisory two consecutive weeks before flipping blocking (per parent plan Locked default).
- **`command-system` scope**: registers the new screenshot command using the standard registry pattern.

### Phase 8 — DEFERRED

Backlog entry filed in this plan's first commit. Re-evaluate when a UE 5.4 install is reachable from the build env. Phase 9 lands without Phase 8 gates.

### Phase 9 — Coverage gates + enforcement

- **Branch**: `feat/test-phase-9-coverage-gates`. Owner: `build-doctor` + `test-author`.
- **Write set**: `scripts/dev/coverage.sh` (NEW, Windows-`OpenCppCoverage`-first with documented `lcov+gcov` POSIX fallback inline), `.github/workflows/coverage.yml` (NEW), `.github/workflows/coverage-gate.yml` (NEW), `CMakePresets.json` (possibly add `ninja-coverage-msys2`).
- **Gate**: per-PR Source_Core change without test delta → blocked unless override label `tests-out-of-band`.
- **POSIX gap documentation**: `scripts/dev/coverage.sh` header carries a comment block — *"OpenCppCoverage is Windows-only. POSIX runners fall back to `lcov+gcov` but the gate ships Windows-runner-only per docs/plans/shipped/test-suite-expansion-completion.md § Locked decisions. Re-evaluate when a POSIX CI runner is provisioned."* PR body repeats this.
- **No Phase 8 dependency** — coverage gate operates on Phases 1-7 + carry-over deltas.

## Risk + halt conditions

Inherits the parent plan's blocking-vs-non-blocking matrix. Halt-only conditions remain:

- `develop` dirty with user-authored uncommitted work → stash, post chip, continue.
- Force-push to `develop` required → backlog whole phase, move on.
- Sanitizer fail on pre-existing prod code → route to `debug-detective` as separate fix-PR; continue test work.

PR D has elevated risk (multi-week interface refactor). Gate: `code-review` flags any non-mechanical semantic change in `OfflineQueueService.cpp` / `TicketSyncService.cpp` as Critical.

## Track B handshake protocol

After PR D ships:

1. Orchestrator updates `docs/plans/active/_plan-locks.md` Track B entry — `on-hold` → `claimed` (or `in-flight` if B1 starts immediately).
2. Track B agent re-asserts B1 write set against the new `IOfflineQueueDeps`-bundle world; expect `OfflineQueueService.cpp` ICache renames to be no-ops post-interface.
3. Track B parallel-allowed against Phase 5 / 6 only if write sets stay disjoint (read `_plan-locks.md` before every Track B slice per AGENTS.md).

## Plan revision contract

Per AGENTS.md § Plan revision after implementation, every PR appends three sections to `docs/plans/shipped/test-suite-expansion.md`:

- `## Implementation log` — `<sha> · <one-line summary>` per PR.
- `## Deviations from plan` — what changed vs this plan + one-line rationale.
- `## Verification` — `ctest` result + mutation-sanity outcomes.

When all carry-overs + Phases 4-7 + Phase 9 ship, move **this** file to `docs/plans/shipped/test-suite-expansion-completion.md` and add a final `## Outcome` table covering all 9 PR batches.

## End-state targets

Unchanged from parent plan, minus Phase 8 (deferred):

- ≥ 70% line coverage on `Source_Core/src/` excluding ImGui/UI files
- ≥ 90% on high-risk units (IssueCreatePipeline, IssueDraft, TrackerFieldValueParser, CallstackParser, LocalCacheManager, TicketSyncService, ConfigManager migrations, MCP dispatch, Lua bindings)
- Phase 9 coverage gate blocks `Source_Core/` diffs without test deltas (override label `tests-out-of-band`)
- Phase 8 UE smoke remains backlogged; flip advisory → blocking after two consecutive green weeks once it ships in a future plan

## Verification (this plan)

This is a planning document — no code shipped here. Verification of the executed PRs lives in the per-PR `## Verification` appendices on the parent plan (`docs/plans/shipped/test-suite-expansion.md`) and on this file's already-archived state under `docs/plans/shipped/`.

## Implementation log

### Session 2026-05-16 (machine A) — 6 PRs shipped

| Slice | PR | Sha | Notes |
|---|---|---|---|
| `callstack-adversarial-subcases` | [#112](https://github.com/alexandrosk0/Smatchet/pull/112) | `effda92` | 4 adversarial subcases + substring pin. ReDoS budget retuned to 1 KiB / 100 ms per MinGW UCRT regex backend (orchestrator spec was 64 KiB / 50 ms — not achievable). |
| `p4blame-parse-tu-split` | [#111](https://github.com/alexandrosk0/Smatchet/pull/111) | `52832d0` | 4 helpers lifted; 12 cases / 75 assertions. |
| `tracker-labels-pure-tu` | [#114](https://github.com/alexandrosk0/Smatchet/pull/114) | `59282a7` | 4 cases / 33 assertions. Agent errored API-500 mid-run; orchestrator recovered. |
| `tracker-datetime-pure-tu` | [#119](https://github.com/alexandrosk0/Smatchet/pull/119) | `9fc5f70` | 7 cases / ~140 assertions. API-500 recovery. |
| `tracker-payload-pure-tu` | [#121](https://github.com/alexandrosk0/Smatchet/pull/121) | `39f91de` | 15 cases / ~233 assertions. API-500 recovery + initial commit missed 3 new files; force-pushed correction. |
| `tracker-field-catalog-pure-tu` | [#122](https://github.com/alexandrosk0/Smatchet/pull/122) | `5ce8def` | 6 cases / ~217 assertions. API-500 recovery + 2-round rebase against sibling Wave A2 merges. |

Plan-lock entries flipped `claimed` → `shipped` via PR [#124](https://github.com/alexandrosk0/Smatchet/pull/124) at `aeaa521`.

### Session boundary — handoff to machine B (first attempt)

- PR D (`offline-queue-deps-interface`) **dispatched then user-stopped** mid-run before any commits landed. Plan-lock entry flipped `claimed` → `abandoned` in the same commit as this log append. Re-claimable on next session — packet template still valid; spec lives at this plan's § PR D.
- 3 git stashes remain on `develop` from concurrent-orchestrator work: `concurrent-agent-leak-A3-C7-B4-WIP`, `external-agent design-doc rename WIP (preserve)`, `theme round-trip doctest (regression guard, unmerged)`. Not mine; do not auto-drop. Inspect on next session.

### Session 2026-05-16 (machine A continuation) — 5 more PRs shipped (10 total) + Phase 4 in flight

Machine-B PC wasn't ready; continued on machine A.

| Slice | PR | Sha | Notes |
|---|---|---|---|
| `offline-queue-deps-interface` (PR D) | [#127](https://github.com/alexandrosk0/Smatchet/pull/127) | `b5fc194` | Re-dispatched after abort. `IOfflineQueueDeps` 7 methods + `ITicketSyncDeps` 17 methods + `AppControllerDepsAdapter`. Zero semantic change. Track B on-hold gate released. |
| `ticket-sync-service-tests` (PR F) | [#130](https://github.com/alexandrosk0/Smatchet/pull/130) | `a618a2f` | 12 cases / 83 assertions. Production bug surfaced: case 3 documents empty-fetch-deletes-all (no `!freshTickets.empty()` guard). Backlog entry filed for follow-up fix. |
| `offline-queue-runtime-tests` (PR E) | [#131](https://github.com/alexandrosk0/Smatchet/pull/131) | `e35794d` | 13 cases / 76 assertions. Required orchestrator-side dedup of test-side `IsTrackerTransportErrorText` mirror after PR F rebase brought `TrackerHttpUtils.cpp` into test target. |
| 8 agent-self-improvement entries | [#128](https://github.com/alexandrosk0/Smatchet/pull/128) | `1fb9753` | Backlog filings from Wave A1+A2. |
| 5 more agent-self-improvement entries | [#132](https://github.com/alexandrosk0/Smatchet/pull/132) | `63c4490` | Backlog filings from PR D+E+F (incl. `TicketSyncService` empty-fetch production bug). |

Plan-lock chore PRs: [#126](https://github.com/alexandrosk0/Smatchet/pull/126) re-claim PR D · [#129](https://github.com/alexandrosk0/Smatchet/pull/129) PR-D shipped + PR-E/F claim · [#132](https://github.com/alexandrosk0/Smatchet/pull/132) PR-E/F shipped · [#133](https://github.com/alexandrosk0/Smatchet/pull/133) Phase 4 claim.

### Session boundary — handoff to machine B (second attempt, 2026-05-16 evening)

- **Phase 4 (`config-schema-migration`)** PR [#134](https://github.com/alexandrosk0/Smatchet/pull/134) **OPEN, CI in_progress** at session end. Agent shipped clean: shared `tests/support/TestEnvGuard.h` + `ConfigManager.test.cpp` (11/42) + `ConfigMigration.test.cpp` (10/57 with 5 [high-risk]) + 4 fixtures + `scripts/dev/test-config-migration.sh`. SmatchetTests aggregate: 284 cases / 1429 assertions. Plan-lock entry stays `in-flight` — machine B merges after CI greens.

### Session boundary — final wrap on machine A (2026-05-16 evening, continuation)

Machine-B remained unavailable; machine A continued briefly:

- **Phase 4 (#134)** **MERGED** at sha `3e19f93`. Plan-lock flipped `in-flight` → `shipped`.
- **Phase 5 (`mcp-json-rpc-harness`)** **DISPATCHED then user-stopped at session end (wrap-up)**. Agent's discovery phase completed before stop and confirmed Phase 5 is **blocked by a production-side prerequisite**: every pure helper in `Plugins/Mcp/` (`BuildRunLuaToolEntry`, `BuildRunLuaSummary`, `BuildToolCallSummary`, `ExtractJsonRpcErrorMessage`, `Base64Encode`, `NormalizeDomain`, `IsLoopbackAddress`, `ConstantTimeStringEquals`, `IsAllowedAttachmentHost`) lives in an anonymous namespace inside a `.cpp` whose top of file pulls `winsock2`, `httplib`, and `cpr`. Tests cannot link the unit without dragging banned deps. Identical pattern to the `P4BlameParse` Phase-2 deferral. Plan-lock entry flipped `claimed` → `abandoned (blocked on production TU split)`. Backlog entry `2026-05-16 · mcp-toolsmith · [infra] — MCP wire-protocol pure logic entombed` filed with full proposal (lift to `Plugins/Mcp/McpJsonRpcPure.{h,cpp}`, ~1-2 h refactor).
- Remaining slices: **Phase 5 pre-flight TU split** (1-2 h `mcp-toolsmith` work, must precede re-dispatch) → Phase 5 (MCP JSON-RPC harness re-dispatch) · Phase 6 (Lua bindings rig) · Phase 7 (screenshot diff) · Phase 9 (coverage gates). Phase 8 still DEFERRED.
- Backlog entries on develop: 14 total (8 from machine-A first half, 5 from D+E+F, 1 new from Phase 5 discovery).
- Active worktrees: only `jolly-cerf-97840e` (concurrent `claude/coordination-plan-locks`) — leave alone.

Test-rig totals on develop at session end: **284 cases / 1429 assertions**.

### Session 2026-05-16 (Phase 5 pre-flight unblocker)

| Slice | PR | Sha | Notes |
|---|---|---|---|
| `mcp-jsonrpc-pure-tu-split` (Phase-5 pre-flight) | [#141](https://github.com/alexandrosk0/Smatchet/pull/141) | `cfab599` | 9 named helpers + 5 transitive (`ToLowerAscii`, `TrimAsciiWhitespace`, `BasenameForDisplay`, `AppendAllowlistedArgKvs`) kept internal-linkage in pure TU + `TruncateOneLine` exported (also called from non-anon code in `McpPlugin.cpp`). Namespace `smatchet::mcp::pure`. Zero semantic change. Test-rig snapshot: 284 cases / 1509 assertions (assertion count grew vs. last session-end snapshot because Phase 4 + downstream landed cases). Unblocks Phase 5 re-dispatch. |
| `test-phase-5-mcp-json-rpc` (Phase 5 re-dispatch) | TBD | TBD | 4 test TUs under `tests/Plugins/Mcp/` covering the 12 exported `smatchet::mcp::pure` helpers: `McpRequestParser.test.cpp` (URL/host parsing + loopback detection), `McpEnvelope.test.cpp` (Base64 + summary builders + JSON-RPC error extraction + TruncateOneLine), `McpToolSchemas.test.cpp` (`run_lua` schema invariants), `McpDispatch.test.cpp` (constant-time compare + attachment-host allowlist). 4 high-risk cases mark assertions whose failure pinpoints specific production lines per backlog #48 (taxonomy option 2 — production-out-of-scope argument-from-assertion-shape). |

### Session 2026-05-16 (Phase 6 unblocker — Lua bindings host interface lift)

| Slice | PR | Sha | Notes |
|---|---|---|---|
| `lua-bindings-host-interface-lift` (Phase 6b pre-flight) | TBD | TBD | Lift `InitLuaCore` body + 11 glue functions out of `AppController_LuaBindings.cpp` (which `#include "imgui.h"` at line 32) into new ImGui-free TU `AppController_LuaBindingsCore.cpp`. New interface `ILuaBindingHost` (in `Source_Core/include/ILuaBindingHost.h`) declares 8 pure-virtual methods + `LuaCommands()` + `AppForCommandContext()` forwarders. `AppController` inherits from `ILuaBindingHost` under `SMATCHET_WITH_LUA_AUTOMATION`. Glues resolve `state["__smatchet_app"]` to `ILuaBindingHost*`; UI glues continue resolving through a separate `state["__smatchet_app_ui"]` (AppController*) key to avoid sol2 v2.20.6's missing base-offset retagging in `get<T*>` under multiple inheritance. Behaviour-preserving — all `LuaCreateIssueBind` / `LuaMcpRegisterToolBind` / `commands.invoke` flows byte-identical pre/post. Standalone + DX12 + `SmatchetLuaTests` + `SmatchetTests` all link clean; ctest 2/2 PASS (including `LuaSandbox.test.cpp` closure invariant). Banned-deps grep (`<imgui|<GLFW|<cpr/|<httplib|<winsock`) empty on both new files. `nm -u` on the new `.obj` shows zero ImGui / GLFW / cpr / httplib / SQLite undefined references. Phase 6b `LuaBindings.test.cpp` re-dispatch unblocked. |

## Deviations from plan

### Session 2026-05-16

- **ReDoS budget** (callstack-adversarial): shipped at 1 KiB / 100 ms instead of plan's 64 KiB / 50 ms — MinGW UCRT `std::regex` backend is super-linear past ~2 KiB and stack-overflows past ~32 KiB. Backlog entry routes regex hardening to `p4-blame`.
- **Auto-merge disabled** on the repo. Orchestrator falls back to direct `gh pr merge --squash --delete-branch` after CI green (or before, when admin-merge allowed). Adds ~14 min wall-clock per PR vs `--auto`. *(2026-05-19: re-enabled via `gh api PATCH repos/alexandrosk0/Smatchet -F allow_auto_merge=true -F delete_branch_on_merge=true`; the AGENTS.md § Merge gates poller remains the canonical autonomous path because it also covers CodeRabbit + user comments, which `--auto` does not.)*
- **Worktree-isolated agent recovery**: 4/4 Wave A2 agents errored API-500 mid-run after shipping 100% of file edits. Orchestrator re-verified gates locally and committed/PR'd from the worktree state. Per-PR cost ~5-10 min vs full re-dispatch.
- **`tests/CMakeLists.txt` rebase cascade**: 4 parallel agents each append to the same lines of the test target source list. Each PR after the first needs a manual rebase resolving the union-merge. Sequential merge order (not parallel) is the right operational stance for this kind of fan-in.
- **Phase-5 pre-flight TU split required before Phase-5 test PR** (added 2026-05-16): pure helpers were entombed inside an anonymous namespace in `Plugins/Mcp/McpPlugin.cpp` whose top of file pulls `winsock2` + `httplib` + `cpr`. Plan originally assumed the helpers were already link-clean — they were not. Pre-flight `mcp-jsonrpc-pure-tu-split` slice lifts them to `Plugins/Mcp/McpJsonRpcPure.{h,cpp}` (cpr/httplib/winsock-free) before Phase 5's test PR can compile.
- **`TruncateOneLine` exported from pure TU** (not in original closure inventory): scan during the lift surfaced 6 call sites in `McpPlugin.cpp`'s non-anon HTTP-handling code. Exporting the helper (rather than duplicating it back into `McpPlugin.cpp`) keeps the post-split TU clean and lets future Phase-5 tests directly cover it.
- **Phase 5 link strategy — Option A (direct `.cpp`)**: `Plugins/Mcp/McpJsonRpcPure.cpp` added directly to the `SmatchetTests` source list (mirroring the Phase 1–4 pattern for `Source_Core/src/*.cpp`) rather than linking against `SmatchetPlugin_Mcp` (Option B). Option A is more surgical — the pure TU pulls only `<string>`, `<cstddef>`, `<nlohmann/json.hpp>`, and `SmatchetDefaults.h`, all already on the test target. Option B would drag the rest of the plugin.
- **No `tests/support/McpJsonRpcFixture.h` shipped**: original packet listed a shared fixture as "if needed". The 4 TUs are self-contained — no JSON fixture files, no shared setup — so the fixture would be dead weight. Dropped per AGENTS.md § Plan-doc safety (do not ship unused files).
- **`__smatchet_app_ui` (AppController*) alongside `__smatchet_app` (ILuaBindingHost*)** in the Phase-6 unblocker: original packet sketch assumed a single `__smatchet_app` key + `static_cast`. Sol2 v2.20.6's `get<T*>` retrieves the raw void* tagged at store time, with no multi-inheritance offset retagging — converting from a stored `ILuaBindingHost*` to `AppController*` via `get<AppController*>` would skew by the base offset and corrupt every UI glue call. Dedicated key keeps each pointer type-safe at its own resolution site. Costs: ~3 lines (one assignment in `InitLuaUi`, one in `AutomationWorkerLoop` for the bgState worker hook, one nil-set in `ClearLuaTicketContextGlue`).
- **`AppForCommandContext()` virtual on `ILuaBindingHost`** for the Phase-6 unblocker `commands.invoke` path: the packet's no-AppController-in-interface rule conflicted with the byte-identical-behaviour rule because the pre-lift `LuaCommandsInvokeGlue` populates `CommandContext::App` with the live AppController. Resolution: `AppController*` is already forward-declared by `Commands/Command.h` and never dereferenced through `ILuaBindingHost`. Production returns `this`; test fakes return `nullptr` (test command handlers either ignore ctx.App or are not registered).
- **Phase 7 — `debug.window.screenshot` already existed** (added 2026-05-16): the original Phase 7 packet asked for a new `debug.window.screenshot` command under `Source_Core/src/Commands/BuiltinCommands.cpp`. Plan-time file-existence scan caught both: (a) the command was added by a prior unrelated slice and is live at `Source_Core/src/Commands/Builtin/BuiltinCommands_Debug.cpp:304`, with the standalone-side PPM writer at `Target_Standalone/main.cpp:569`; (b) `BuiltinCommands.cpp` is a thin registry-wiring shim post the Builtin-* file split. The Phase 7 slice consequently shipped zero new CLI command surface — only the two new scenarios + a tiny `requestCommandPalette*` flag pair so the palette scenario can drive the modal open/filter without reaching into `SmatchetUI`'s private `commandPalette_`. Plan packet's write set narrowed accordingly (recorded in `docs/plans/active/_plan-locks.md`).
- **Phase 7 — pink-clear pink-pixel scan deferred** (added 2026-05-16): the packet floated using `glClearColor(1, 0, 1, 1)` during the dock-gap scenario so any visible magenta in the PPM = dock gap. Toggling clear color requires a new `UiDrawSession::requestClearColor` flag + main-loop consumer — adding it doubled the slice's surface for marginal extra coverage given that the L∞ diff against a clean golden already catches dock-gap shifts. The pink-pixel scan helper `smatchet::test::CountPixels` ships in `GoldenImage.h` so a future scenario can flip the clear color and run the scan with two extra lines of bash. Backlog entry filed.
- **Phase 7 — `tests/support/screenshot_diff_main.cpp` compiled by bash, not CMake** (added 2026-05-16): packet implied integration with the doctest rig. Wiring the diff helper as a CMake target would have grown `tests/CMakeLists.txt` for a single throwaway CLI binary that only the bash driver uses. The helper is `g++ -std=c++14 -O2` compiled on-demand inside `test-screenshot-diff.sh`; the production / doctest builds stay untouched. Override via `SCREENSHOT_DIFF_BIN=…` env var for a pre-built helper.
- **Phase 7 — mutation-sanity recipe deferred to follow-up** (added 2026-05-16): packet asked to introduce a 1-pixel dock-spacing offset, observe the gate fail, then revert before commit. The two new scenarios + bash gate need the first **golden capture run on the user's machine** before the mutation recipe is meaningful (a brand-new golden is byte-equal to its own capture by construction; the mutation has to follow the bootstrap). Recipe is documented inline in `scripts/dev/test-screenshot-diff.sh`'s header comment; full mutation-sanity demo is filed as a backlog follow-up — to be exercised against the bootstrapped goldens in the next session.
- **Phase 9 — coverage threshold (≥ 70%) advisory not blocking** (added 2026-05-16): parent plan's § End-state targets calls for ≥ 70% line coverage on `Source_Core/src/` as a hard gate. Phase 9 ships the **measurement** (capture + report + Cobertura artifact + delta gate) but invokes `coverage.sh --threshold 0` in CI so the numerical threshold can be tuned against a real two-week distribution before flipping to blocking. Same advisory → blocking lifecycle as Phase 7's screenshot-diff. Threshold-flip is its own future PR — `coverage.yml` carries a header comment naming the flip path.
  - **FLIPPED advisory → blocking 2026-06-04** via `docs/plans/shipped/coverage-threshold-graduation.md`: `continue-on-error: false` + `--threshold 65`. The flip's measure-first step exposed that OCC's `--sources` patterns were dotted (regex-shaped) but OCC matches by **substring** → the gate had been measuring **0 lines** the whole soak (issue #833, fixed in the same effort). First real measurement on the Ui-excluded surface = **67%**, so the gate graduated at a **floor of 65** (not the 70 target) with a raise-to-70 ramp backlogged (`categories/test.md`).
- **Phase 9 — structural delta gate ships hard-blocking from day 1** (added 2026-05-16): unlike the numerical threshold, the *structural* gate ("if you change `Source_Core/src/*.cpp`, add a test or apply `tests-out-of-band`") is hard-blocking from the first PR. Rationale: the structural check is binary and unambiguous, doesn't suffer from CI-runner-noise the way numerical gates do, and the `tests-out-of-band` label provides a low-friction escape hatch for legitimate exceptions. Soaking it would just teach contributors to ignore the gate.
- **Phase 9 — `tests-out-of-band` label dismisses gate** (added 2026-05-16): the override label exists for legitimate cases (docs-only `Source_Core/include/` comment edits, include-shape fixes for dual-target compatibility, sanitizer flag flips, build-system PRs that touch `Source_Core/` transitively). Label inspection lives in the workflow (`gh pr view --json labels`), not the bash gate — the bash script only signals the missing-test condition. Label must be created at the repo level (not provided by default) — surfaced as residue for user.
- **Phase 9 — OpenCppCoverage local install not blocking the slice** (added 2026-05-16): packet allows shipping without local OpenCppCoverage. CI runners install via Chocolatey (`choco install opencppcoverage`); local dev exposure is opt-in. `coverage.sh` exits 2 with a clear install hint when the binary is absent — both the missing-binary branch and the present-binary branch are acceptable slice-gate outcomes.
- **Phase 9 — `--coverage` linker flags duplicated into preset** (added 2026-05-16): MSYS2 GCC needs `--coverage` on both the compiler **and** linker invocations to produce `.gcno` / `.gcda` files. CMake doesn't propagate `CMAKE_CXX_FLAGS` to link automatically, so the preset sets `CMAKE_EXE_LINKER_FLAGS` + `CMAKE_SHARED_LINKER_FLAGS` explicitly. OpenCppCoverage itself works against uninstrumented PDBs too; the gcov flags are insurance for the documented `lcov+gcov` POSIX fallback path.

## Verification

### Phase 5 pre-flight (`mcp-jsonrpc-pure-tu-split`) — 2026-05-16

- **Standalone build**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone` → clean (`Smatchet.exe` linked, `EXIT=0`).
- **DX12 build**: `cmake --build --preset ninja-iter-msys2 --target SmatchetCore_DX12` → clean (`libSmatchetCore_DX12.a` linked, `EXIT=0`).
- **Test rig**: `cmake --build --preset ninja-test-msys2 && ctest --output-on-failure` → 1/1 ctest target passed; doctest summary `284 cases | 284 passed | 0 failed | 1509 assertions | 1509 passed | 0 failed | Status: SUCCESS!`.
- **Banned-deps guard**: `grep -nE '#include\s*<(httplib|cpr|winsock2)' Plugins/Mcp/McpJsonRpcPure.{h,cpp}` → no matches (empty output). Pure TU is link-clean for the test rig.

### Phase 5 re-dispatch (`test-phase-5-mcp-json-rpc`) — 2026-05-16

- **Test rig**: `cmake --build --preset ninja-test-msys2 && ctest --output-on-failure` → 1/1 ctest target passed; doctest aggregate captured in PR body (≥ baseline 284 cases / 1509 assertions + Phase-5 contribution).
- **Dual-target regression**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` → clean (production untouched, smoke check).
- **Sidecar suite**: `bash scripts/dev/test-all.sh` → green.
- **Manual residue**: none (pure-logic harness, fully automated).

### Phase 6 (`test-phase-6-lua-bindings`) — 2026-05-16

Three of four planned test TUs shipped under a new `SmatchetLuaTests` binary (gated by `SMATCHET_BUILD_LUA_TESTS=ON`, auto-enabled in `ninja-test-msys2` preset). The fourth — `LuaBindings.test.cpp` — deferred behind a Class C production prerequisite.

- **InitLuaCore classification**: Class C per `docs/plans/active/_plan-locks.md`. `AppController_LuaBindings.cpp:32` `#include "imgui.h"` + `:766` `state["__smatchet_app"] = this` + the `smatchet_lua_init_detail::*Glue` functions resolving `__smatchet_app -> AppController*` at every call → cannot link the binding TU into a test target. Production-side TU split + interface lift required first (backlog entry `2026-05-16 · lua-binder · [infra]` filed with proposal).
- **Shipped TUs**: `tests/Lua/LuaSandbox.test.cpp` (7 cases — sandbox closure invariant: denied globals are absent + allowed globals are present + os.* whitelist is exactly {time, clock, difftime, date} + safe-script error path returns recoverable result; 1 [high-risk]), `tests/Lua/LuaTimeout.test.cpp` (4 cases — count-hook abort + budget-respect + abort-not-panic + reinstall-across-scripts; 1 [high-risk]), `tests/Lua/LuaStubsCompile.test.cpp` (3 cases — public-surface symbol-name drift sentinel for the binding ↔ stub pair).
- **Shared support**: `tests/support/LuaHostFixture.h` mirrors production `InitLuaCore`'s lib opens + os whitelist + panic-as-exception hook. Single source of truth for sandbox + timeout invariants on the test side.
- **Build integration**: new `tests/Lua/CMakeLists.txt`. Links only `Smatchet_Lua_Internal` + `doctest::doctest`; NO ImGui, NO cpr, NO SQLite. Per-source `SOL_ALL_SAFETIES_ON=1` mirrors `AppController.h:10`.
- **Mutation-sanity**: 2 high-risk cases (sandbox closure + timeout-abort-not-panic). Each ships an inline comment naming which production line/branch the assertion forces, per backlog #48 taxonomy path 2.

### Phase 6 unblocker (`lua-bindings-host-interface-lift`) — 2026-05-16

- **Standalone build**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone` → clean (`Smatchet.exe` linked).
- **DX12 build**: `cmake --build --preset ninja-iter-msys2 --target SmatchetCore_DX12` → clean (`libSmatchetCore_DX12.a` linked).
- **Test rig**: `cmake --build --preset ninja-test-msys2 && ctest --output-on-failure` → 2/2 ctest targets passed (SmatchetTests + SmatchetLuaTests). `LuaSandbox.test.cpp` closure invariant remains green (7 cases / 99 assertions in `SmatchetLuaTests`).
- **Banned-deps grep**: `grep -nE '#include\s*<(imgui|GLFW|cpr/|httplib|winsock)' Source_Core/src/AppController_LuaBindingsCore.cpp Source_Core/include/ILuaBindingHost.h` → empty.
- **Undefined-symbol audit**: `nm -u build/ninja-iter-msys2/CMakeFiles/SmatchetStandalone.dir/Source_Core/src/AppController_LuaBindingsCore.cpp.obj | grep -iE 'imgui\|glfw\|cpr\|httplib\|sqlite'` → empty. New TU has zero ImGui / GLFW / cpr / httplib / SQLite undefined refs.
- **Sidecar suite**: `bash scripts/dev/test-all.sh` → 99 passed / 8 failed; the 8 failures are pre-existing `test-lint-hook-split` infra failures (worktree lacks `.claude/hooks/` setup, documented in PR #142). Unrelated to this slice.
- **Behaviour-equivalence smoke**: deferred to manual residue (bucket-E ImGui Test Engine not yet wired — pillar 4 gap noted in `docs/backlog/AGENT_SELF_IMPROVEMENT.md` category `context`). Behaviour-preservation is enforced by the byte-identical glue bodies (modulo cast site) + the LuaSandbox closure invariant.

### Phase 6b (`test-phase-6b-lua-bindings-roundtrip`) — 2026-05-16

Closes the Phase 6 split. The deferred `LuaBindings.test.cpp` now ships against the PR #144 interface lift; production untouched.

- **Write set**: `tests/Lua/LuaBindings.test.cpp` (NEW), `tests/support/FakeLuaBindingHost.h` (NEW), `tests/Lua/CMakeLists.txt` (append-only — add the new test TU + `AppController_LuaBindingsCore.cpp` + ConfigManager / Logger / FieldEditAuditSource / Commands::* transitive .cpps, plus the `-mcmodel=large` source-property mirror).
- **Coverage**: 13 cases on the round-trip surface — `smatchet.get_ticket` happy path + miss path (2 [high-risk]); `smatchet.get_active_tickets`; `log_info`; `decode_json` valid + invalid; `Ticket:set_field` accept + reject + unknown-field paths (1 [high-risk]); `Ticket:transition`; `mcp.register_tool`; `commands.invoke` unknown + routed paths; top-level table presence; `tracker.get_type` shape; sandbox-closure regression smoke.
- **Fake design**: `FakeLuaBindingHost` implements the 8 `ILuaBindingHost` virtuals + `LuaCommands()` / `AppForCommandContext()`. Knobs (`TicketsById`, `FieldsById`, `SubmitFieldEditReturn` / `Error`, `CreateIssueScripter`) drive return values; recording fields (`LoggedInfo`, `SubmitFieldEditCalls`, `McpRegistrations`, `CreateIssueSpecKeys`) drive observability. Header-only; one fixture per `TEST_CASE`; no static leakage.
- **Mutation-sanity**: 3 high-risk cases each carry an argue-from-shape comment naming the production line of `AppController_LuaBindingsCore.cpp` the assertion forces. Production-mutation deferred per backlog #48 taxonomy path 2.
- **Build integration**: `tests/Lua/CMakeLists.txt` adds nlohmann_json + crypt32 (Windows) link deps. Production `AppController_LuaBindingsCore.cpp` compiles with `-mcmodel=large` in the test target too, mirroring `CMakeLists.txt:983-988`.

### Phase 7 (`test-phase-7-screenshot-diff`) — 2026-05-16

Bucket-C verification shipped. Two new `IScenario` subclasses drive the standalone UI to a known steady state, then trigger the **pre-existing** `debug.window.screenshot` flag pair (already wired in `BuiltinCommands_Debug.cpp:304` + `Target_Standalone/main.cpp:569`) so a bash driver can diff captured PPMs against checked-in goldens.

- **Standalone build**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone` → expected clean. Two new scenario .cpps auto-picked-up by `file(GLOB_RECURSE CORE_SOURCES …)` at `CMakeLists.txt:532`; no CMake edit required.
- **DX12 build**: `cmake --build --preset ninja-iter-msys2 --target SmatchetCore_DX12` → expected clean. Scenarios only touch `AppController` + `UiDrawSession`; the screenshot capture path (`glReadPixels`) stays in `Target_Standalone/main.cpp` so DX12 compiles the scenario TUs but never runs the capture (request flag is set but unconsumed on Unreal).
- **Test rig**: `cmake --build --preset ninja-test-msys2 && ctest --output-on-failure` → expected clean. New `tests/support/GoldenImage.h` is header-only and not yet consumed by the doctest target; it's compiled by the bash driver's helper.
- **Bash driver**: `bash scripts/dev/test-screenshot-diff.sh` — auto-enrolled by `scripts/dev/test-all.sh`. Two scenarios (`dock-gap-sentinel`, `command-palette-fuzzy`), per-channel L∞ tolerance = 4. First run on a clean checkout auto-bootstraps the goldens and PASSes with a `WARN` line; subsequent runs gate.
- **CI workflow**: `.github/workflows/build-and-test.yml` appends a `continue-on-error: true` step running the driver. Advisory until 2026-05-30 (two-week soak), then flips to blocking per parent plan's Locked default.
- **Manual residue**: none for the bucket-C surface itself. DX12 backbuffer readback for screenshot-diff is filed as a follow-up `unreal-bridge` backlog entry. Headless CI runners lack a usable GL context for `--spawn` UI sessions — backlog entry filed for `build-doctor` to wire mesa/xvfb (until then the advisory CI step legitimately no-ops in the cloud and the gate runs on dev machines).

### Phase 9 (`test-phase-9-coverage-gates`) — 2026-05-16

Coverage measurement + structural test-delta enforcement infra. Pure infra slice — no production code change, no new tests. The slice ships the gate around the 366 cases / 1820 assertions already in `SmatchetTests` + `SmatchetLuaTests` + screenshot diff from Phases 1-7.

- **CMake preset configure**: `cmake -B build/ninja-coverage-msys2 --preset ninja-coverage-msys2` → expected clean. New preset inherits `_smatchet-msys2-base`, enables `SMATCHET_BUILD_TESTS` + `SMATCHET_BUILD_LUA_TESTS`, sets `--coverage -fprofile-arcs -ftest-coverage` on compiler + linker.
- **Instrumented test build**: `cmake --build --preset ninja-coverage-msys2 --target SmatchetTests SmatchetLuaTests` → expected clean. gcov instrumentation adds ~10% wall-clock + emits `.gcno` next to each `.obj`.
- **ctest under instrumentation**: `(cd build/ninja-coverage-msys2 && ctest --output-on-failure)` → expected 2/2 PASS (instrumentation doesn't change behaviour; emits `.gcda` profile data on run).
- **Coverage capture (when OpenCppCoverage available)**: `bash scripts/dev/coverage.sh` runs both test exes through OpenCppCoverage, writes `coverage/coverage.xml` (Cobertura) + `coverage/coverage-html/index.html`. Reports line coverage percentage to stdout. When OpenCppCoverage absent locally, exits 2 with install hint — acceptable slice gate outcome.
- **Delta gate**: `bash scripts/dev/coverage-delta-gate.sh` — Phase 9 PR itself touches zero `Source_Core/src/*.cpp` files, so the `|prod| == 0` short-circuit fires and the gate PASSes. Exit 0.
- **Non-coverage tests path regression**: `cmake --build --preset ninja-test-msys2 && (cd build/ninja-test-msys2 && ctest --output-on-failure)` → expected clean — adding the new preset doesn't affect the existing presets.
- **Dual-target regression smoke**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` → expected clean (no `Source_Core/` change).
- **Sidecar suite**: `bash scripts/dev/test-all.sh` → expected green (8 pre-existing failures noted in Phase 6 verification remain unchanged). `coverage.sh` and `coverage-delta-gate.sh` deliberately NOT auto-enrolled — they're CI gates, not pre-merge dev smoke. The auto-enrolment naming convention is `test-*.sh`; the coverage scripts use the `coverage*` prefix to opt out.
- **Manual residue**: (a) `tests-out-of-band` GitHub label must be created at the repo (no API surface in this slice — `gh label create tests-out-of-band --description "Bypass coverage delta gate for non-behavioural Source_Core/ changes"`); (b) OpenCppCoverage local install is opt-in for dev (CI installs via Chocolatey); (c) PR template addition documenting the override label is filed as backlog follow-up (no existing `.github/pull_request_template.md`).

## Implementation log

| Slice | PR | Sha | Notes |
|---|---|---|---|
| `test-phase-7-screenshot-diff` (Phase 7) | [#146](https://github.com/alexandrosk0/Smatchet/pull/146) | `d857310` | Added `DockGapSentinelScenario` + `CommandPaletteFuzzyScenario` + `tests/support/GoldenImage.h` (header-only PPM + L∞ diff) + `tests/support/ScreenshotDiffMain.cpp` (bash-compiled helper) + `scripts/dev/test-screenshot-diff.sh` + `.github/workflows/build-and-test.yml` advisory step. `UiDrawSession::requestCommandPaletteOpen` + `requestCommandPaletteFilter` flag pair lets the palette scenario open + pre-filter without touching `SmatchetUI`'s private `commandPalette_`. Re-uses pre-existing `debug.window.screenshot` flag pair (no new CLI command surface — plan packet's `BuiltinCommands.cpp` delta was satisfied by prior infra). Goldens auto-bootstrap on first run; the second run gates. |
| `test-phase-9-coverage-gates` (Phase 9) | [#148](https://github.com/alexandrosk0/Smatchet/pull/148) | `039d286` | Wires the coverage measurement + structural-test-delta enforcement infra: `scripts/dev/coverage.sh` (Windows-OpenCppCoverage-first wrapper; POSIX `lcov+gcov` fallback documented inline per § Locked decisions), `scripts/dev/coverage-delta-gate.sh` (per-PR `Source_Core/src/*.cpp` changes without test deltas → blocked unless `tests-out-of-band` label), `.github/workflows/coverage.yml` (advisory threshold for 2-week soak), `.github/workflows/coverage-gate.yml` (hard-blocking test-delta check from day 1; label override via `gh pr view --json labels`), new `ninja-coverage-msys2` CMake preset (gcov-instrumented build/test pair). Threshold enforcement (≥ 70% line coverage on `Source_Core/src/`) deliberately disabled (`--threshold 0`) for the soak — future flip is its own PR. OpenCppCoverage installed on the CI runner via Chocolatey; documented local install hint when absent. |

## Outcome

Plan-execution summary. Phases 1-7 + 9 shipped; Phase 8 (UE 5.4 smoke) remains deferred to backlog (`unreal-bridge · [infra]`) pending a UE install reachable from the build env.

| Phase / Slice | PR | Sha | Cases Δ | Assertions Δ |
|---|---|---|---|---|
| Wave A1 — `callstack-adversarial-subcases` | [#112](https://github.com/alexandrosk0/Smatchet/pull/112) | `effda92` | +1 | +9 |
| Wave A1 — `p4blame-parse-tu-split` | [#111](https://github.com/alexandrosk0/Smatchet/pull/111) | `52832d0` | +12 | +75 |
| Wave A2 — `tracker-labels-pure-tu` | [#114](https://github.com/alexandrosk0/Smatchet/pull/114) | `59282a7` | +4 | +33 |
| Wave A2 — `tracker-datetime-pure-tu` | [#119](https://github.com/alexandrosk0/Smatchet/pull/119) | `9fc5f70` | +7 | +140 |
| Wave A2 — `tracker-payload-pure-tu` | [#121](https://github.com/alexandrosk0/Smatchet/pull/121) | `39f91de` | +15 | +233 |
| Wave A2 — `tracker-field-catalog-pure-tu` | [#122](https://github.com/alexandrosk0/Smatchet/pull/122) | `5ce8def` | +6 | +217 |
| PR D — `offline-queue-deps-interface` | [#127](https://github.com/alexandrosk0/Smatchet/pull/127) | `b5fc194` | 0 | 0 |
| PR F — `ticket-sync-service-tests` | [#130](https://github.com/alexandrosk0/Smatchet/pull/130) | `a618a2f` | +12 | +83 |
| PR E — `offline-queue-runtime-tests` | [#131](https://github.com/alexandrosk0/Smatchet/pull/131) | `e35794d` | +13 | +76 |
| Phase 4 — `config-schema-migration` | [#134](https://github.com/alexandrosk0/Smatchet/pull/134) | `3e19f93` | +21 | +99 |
| Phase 5 preflight — `mcp-jsonrpc-pure-tu-split` (unblocker) | [#141](https://github.com/alexandrosk0/Smatchet/pull/141) | `cfab599` | 0 | 0 |
| Phase 5 — `mcp-json-rpc-harness` | [#142](https://github.com/alexandrosk0/Smatchet/pull/142) | `d0b1f12` | +23 | +145 |
| Phase 6 — Lua sandbox + timeout + stubs-compile (partial) | [#143](https://github.com/alexandrosk0/Smatchet/pull/143) | `ba1302e` | +14 | +99 |
| Phase 6 unblocker — `lua-bindings-host-interface-lift` | [#144](https://github.com/alexandrosk0/Smatchet/pull/144) | `7e6762d` | 0 | 0 |
| Phase 6b — `lua-bindings-roundtrip` (closes Phase 6 split) | [#145](https://github.com/alexandrosk0/Smatchet/pull/145) | `d125b36` | +15 | +63 |
| Phase 7 — `screenshot-diff` (bucket C) | [#146](https://github.com/alexandrosk0/Smatchet/pull/146) | `d857310` | +4 scenarios | n/a (bucket C) |
| Phase 9 — `coverage-gates` | [#148](https://github.com/alexandrosk0/Smatchet/pull/148) | `039d286` | 0 | 0 |
| **Total (doctest)** | | | **+143 cases** | **+1272 assertions** |

Out-of-band PR shipped during the run (not part of the original plan, surfaced by PR F):

| Slice | PR | Sha | Notes |
|---|---|---|---|
| `offline-sync` — TicketSyncService empty-fetch full-sync cache-wipe guard | [#139](https://github.com/alexandrosk0/Smatchet/pull/139) | `95d51a5` | Production bug surfaced as `[high-risk]` documented-bug case 3 in PR F's `TicketSyncService.test.cpp`. Guard added: `if (fullSyncCompleted && !freshTickets.empty())` around the stale-deletion branch. Test case flipped from documenting the bug to enforcing the guard. Backlog entry `2026-05-16 · offline-sync · [bug]` flipped to applied. |

### End-state vs targets

- **Aggregate test coverage** on develop at plan close: `SmatchetTests` 322 cases / 1717 assertions + `SmatchetLuaTests` 29 cases / 162 assertions + 4 screenshot scenarios = 355 test cases backing `Source_Core/src/` + `Plugins/Mcp/` + `Plugins/Lua` + bucket-C visual.
- **Line coverage threshold** (≥ 70% on `Source_Core/src/` excluding ImGui/UI): infrastructure shipped via Phase 9 (`OpenCppCoverage` wrapper + Cobertura artifact); threshold flip from advisory (`--threshold 0`) to hard-blocking (`--threshold 70`) deferred to its own future PR after the standard 2-week advisory soak (target flip: 2026-05-30).
- **Structural test-delta gate**: hard-blocking from day 1. `Source_Core/src/*.cpp` changes without test deltas → CI fails unless `tests-out-of-band` PR label is present (label must be created at repo via `gh label create tests-out-of-band` — surfaced as residue in PR #148).
- **Screenshot diff (bucket C)**: 2 seed scenarios live (dock-gap-sentinel + command-palette-fuzzy), per-channel L∞ ≤ 4 tolerance, advisory CI step (`continue-on-error: true`) until 2026-05-30 then flips blocking.
- **Phase 8 (UE 5.4 smoke)**: DEFERRED. Re-evaluate when a UE 5.4 install is reachable from the build env. Phase 9 shipped without Phase 8 dependencies per § Risk + halt conditions.
- **Backlog follow-ups** filed during the plan run (open items, not blocking plan closure):
  - `unreal-bridge · [infra]` — DX12 backbuffer readback for screenshot diff
  - `build-doctor · [tooling]` — headless CI runner GL context (mesa / ANGLE-D3D11) for bucket-C / bucket-E in cloud
  - `build-doctor · [tooling]` — flip coverage threshold advisory → blocking after 2-week soak
  - `build-doctor · [infra]` — OpenCppCoverage local install hint in `docs/harness/SETUP.md`
  - `test-author · [tooling]` — `.github/pull_request_template.md` with `## Coverage gate override` section
  - `test-author · [tooling]` — Phase 7 mutation-sanity demo against bootstrapped goldens (recipe in `scripts/dev/test-screenshot-diff.sh` header)
  - `test-author · [tooling]` — Phase 7 pink-clear dock-gap pixel-scan follow-up (`smatchet::test::CountPixels` shipped, awaits clear-color toggle)
  - `lua-binder · [context]` — sol2 v2.20.6 multi-inheritance base-offset retagging gotcha documented in `agents/lua-binder.md` § sol2 v2.20.6 API constraints
- **Operational improvements** locked in during the plan run (already applied):
  - Lint-hook split: PostToolUse inline (`clang-format -i` only) + Stop drain (`cppcheck` + `clang-tidy` + dual-target) with auto-cleared `.tree-dirty` sentinel. Reduced per-edit lint cost from 5–15 s to <1 s.
  - Slice-boundary build rule: at most one `cmake --build` + one `ctest` per agent turn, only after implementation is complete. Built into the test-rig / build-doctor agent prompts.
  - Trivial-visual-only change envelope: theme/locale literal-only edits skip the full regression loop, ship after a single Standalone build.
  - TU-split pre-authorisation pattern: orchestrator delegation packets for `test-rig` carry explicit allowed-production-file lists for the `<Unit>Parse.{h,cpp}` lift class (Phase 1 `IssueCreatePipelineHelpers` set the recipe; carried forward through Wave A1/A2 + Phase 5 preflight + Phase 6 unblocker).

**Plan status**: closed. Moved to `docs/plans/shipped/test-suite-expansion-completion.md` via the same chore PR that flipped Phase-9's plan-lock to shipped. Subsequent test-coverage work is per-slice (e.g. the threshold flip, the DX12 readback, the bucket-E ImGui Test Engine wiring) and originates from individual backlog entries rather than this multi-phase plan.

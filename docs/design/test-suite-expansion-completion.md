# Test suite expansion — completion plan (Phases 4-9 + carry-overs)

> Continuation of [`docs/design/applied/test-suite-expansion.md`](applied/test-suite-expansion.md). Phases 1-3 shipped 2026-05-16 ([#103](https://github.com/alexandrosk0/Smatchet/pull/103), [#106](https://github.com/alexandrosk0/Smatchet/pull/106), [#107](https://github.com/alexandrosk0/Smatchet/pull/107)). This plan sequences the remaining work end-to-end with locked decisions, parallelisation rules, and per-slice agent packets.

## Locked decisions (2026-05-16 user review)

| Decision | Value | Notes |
|---|---|---|
| Phase 8 — UE 5.4 smoke | **Defer** | No reachable UE install on build env. Backlog entry `infra` / blocker "no UE install". Phase 9 lands without Phase 8 gates. Re-evaluate when UE install lands. |
| `large-files-and-phase-2` Track B | **Resume early** — after PR D (`offline-queue-deps-interface`) lands | Original gate was "Phase 9 ships". PR D removes the largest Track B conflict (`OfflineQueueService.cpp` + `AppController.h`); Track B may resume against narrower-than-umbrella write set after D. |
| Coverage tool (Phase 9) | **`OpenCppCoverage` Windows-only**, `lcov+gcov` documented fallback | No POSIX runner today. Acceptable per user. Document POSIX gap in `scripts/dev/coverage.sh` header + Phase 9 PR body. |
| Carry-overs | **Ship all first**, then Phase 4-9 sequential | Eliminates concurrent-PR conflict risk during forward phases. Saves rebase cost on Phase 4-9 vs interleaved alternative. |

Plus existing locked defaults from [§ Locked defaults](applied/test-suite-expansion.md#locked-defaults--no-clarifications-needed) of the parent plan (still in force): `TrackerHttpClient` interface shim, GL-context CI policy, mutation testing out of scope, `IScenario` reuse, ImGui Test Engine extension, blocking-vs-non-blocking deferral protocol.

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

1. **Plan-lock pre-flight** — *"Read `docs/design/_plan-locks.md` first. Refuse if your write set overlaps an `in-flight` or `claimed` entry; surface to the orchestrator."* Orchestrator computes intersection and writes the new claim before dispatch.
2. **Write set bounds** explicit — narrow paths per slice; no umbrella claims.
3. **Test-rig `<Unit>Parse.{h,cpp}` TU-split pre-authorisation** for any unit with anon-namespace pure helpers (per AGENTS.md applied 2026-05-16 rule).
4. **Plan-time production-file existence check** (5 s Glob) before finalising the packet (per AGENTS.md applied 2026-05-16 rule).
5. **Output budget** — `Report ≤ 200 words, table form, no prose paragraphs`.
6. **Plan revision contract** — append to `docs/design/applied/test-suite-expansion.md` § Implementation log + § Deviations + § Verification in the same commit per AGENTS.md § Plan revision after implementation.
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
- **Track B handshake**: after PR D merges, update `docs/design/_plan-locks.md` Track B entry — `on-hold` → `claimed` (or `in-flight` if user resumes B1 immediately). B1 (`ICacheAccess.h`) write set no longer overlaps `OfflineQueueService.h`.

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
- **POSIX gap documentation**: `scripts/dev/coverage.sh` header carries a comment block — *"OpenCppCoverage is Windows-only. POSIX runners fall back to `lcov+gcov` but the gate ships Windows-runner-only per docs/design/test-suite-expansion-completion.md § Locked decisions. Re-evaluate when a POSIX CI runner is provisioned."* PR body repeats this.
- **No Phase 8 dependency** — coverage gate operates on Phases 1-7 + carry-over deltas.

## Risk + halt conditions

Inherits the parent plan's blocking-vs-non-blocking matrix. Halt-only conditions remain:

- `develop` dirty with user-authored uncommitted work → stash, post chip, continue.
- Force-push to `develop` required → backlog whole phase, move on.
- Sanitizer fail on pre-existing prod code → route to `debug-detective` as separate fix-PR; continue test work.

PR D has elevated risk (multi-week interface refactor). Gate: `code-review` flags any non-mechanical semantic change in `OfflineQueueService.cpp` / `TicketSyncService.cpp` as Critical.

## Track B handshake protocol

After PR D ships:

1. Orchestrator updates `docs/design/_plan-locks.md` Track B entry — `on-hold` → `claimed` (or `in-flight` if B1 starts immediately).
2. Track B agent re-asserts B1 write set against the new `IOfflineQueueDeps`-bundle world; expect `OfflineQueueService.cpp` ICache renames to be no-ops post-interface.
3. Track B parallel-allowed against Phase 5 / 6 only if write sets stay disjoint (read `_plan-locks.md` before every Track B slice per AGENTS.md).

## Plan revision contract

Per AGENTS.md § Plan revision after implementation, every PR appends three sections to `docs/design/applied/test-suite-expansion.md`:

- `## Implementation log` — `<sha> · <one-line summary>` per PR.
- `## Deviations from plan` — what changed vs this plan + one-line rationale.
- `## Verification` — `ctest` result + mutation-sanity outcomes.

When all carry-overs + Phases 4-7 + Phase 9 ship, move **this** file to `docs/design/applied/test-suite-expansion-completion.md` and add a final `## Outcome` table covering all 9 PR batches.

## End-state targets

Unchanged from parent plan, minus Phase 8 (deferred):

- ≥ 70% line coverage on `Source_Core/src/` excluding ImGui/UI files
- ≥ 90% on high-risk units (IssueCreatePipeline, IssueDraft, TrackerFieldValueParser, CallstackParser, LocalCacheManager, TicketSyncService, ConfigManager migrations, MCP dispatch, Lua bindings)
- Phase 9 coverage gate blocks `Source_Core/` diffs without test deltas (override label `tests-out-of-band`)
- Phase 8 UE smoke remains backlogged; flip advisory → blocking after two consecutive green weeks once it ships in a future plan

## Verification (this plan)

This is a planning document — no code shipped here. Verification of the executed PRs lives in the per-PR `## Verification` appendices on the parent plan (`docs/design/applied/test-suite-expansion.md`) and on this file's eventual move to `applied/`.

## Implementation log

(Append per shipped PR — `<sha> · <one-line summary>`.)

## Deviations from plan

(Append per shipped PR — what changed vs this plan + one-line rationale.)

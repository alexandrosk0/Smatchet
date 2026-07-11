# Plan — God-file splits (architecture review item 4)

> **Slug**: `god-file-splits` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — all five splits merged (#1732, #1741, #1745, #1756, #1759) + the tu-line-ceiling lint.

<!-- index-summary: Mechanical, behavior-preserving partition of the five largest TUs (MarkdownConvert, ConfigManager, CliCommandRunner, AppController_LuaBindings, SmatchetActiveProjectGridUi) into cohesive companion TUs, plus an advisory tu-line-ceiling lint so the class doesn't regrow. -->

## Context

The 2026-07 codebase architecture review flagged five god files as its item 4:
`Source/Core/src/Ui/MarkdownConvert.cpp` (2,047 LOC), `Source/Standalone/CliCommandRunner.cpp`
(1,871), `Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp` (1,741),
`Source/Core/src/Config/ConfigManager.cpp` (1,640), and
`Source/Core/src/AppController_LuaBindings.cpp` (1,561). Each concatenates several cohesive
engines into one TU, amplifying merge conflicts, review scope, and (for the Lua-bindings file)
the security-review surface of the `ai.*` off-UI-thread invariant. The repo already fixed the
review's items 3 (PR #1708) and 6 (PR #1710); sibling campaigns cover items 1, 2, and 5. After
this plan lands, every targeted TU is a cohesive partition under the new 1,200-line advisory
ceiling (most land at or below ~700 LOC) and the lint warns before any first-party TU regrows
past it.

**Hard environment constraint** (same as
[`docs/plans/appcontroller-clusters-followup.md`](../appcontroller-clusters-followup.md)):
`posix-core-check` cannot configure in the authoring container (session egress policy blocks the
`cpr`/`curl` FetchContent tarball), so **CI (Windows+MSVC dual-target build + ctest + UI lanes) is
the sole correctness gate** for every slice.

## Approach

One god file per PR, lowest-risk first: **MarkdownConvert → ConfigManager → CliCommandRunner →
AppController_LuaBindings → SmatchetActiveProjectGridUi**, with the regression lint riding the
final PR. Every slice is a byte-identical body move: the original file stays as the "spine"
(public entry points + composition), cohesive clusters move to companion TUs named by the existing
`<File>_<Area>.cpp` convention (`ConfigManager_Panes.cpp` precedent), and state shared across new
TUs moves to a `<File>_Internal.h` (`ConfigManager_Internal.h` precedent). Declarations in public
headers are untouched — callers, linkage, and behavior are identical; only which `.o` a symbol
lands in changes. New TUs auto-join the build via the per-component `file(GLOB …)` source lists
(post PR #1710); no CMake edits.

The trade-off that shaped sequencing: MarkdownConvert and ConfigManager lead because they carry
the strongest doctest nets; the grid UI file goes last because its only nets are the UI-smoke and
monkey lanes. The Lua-bindings slice is sequenced behind the AppController fan-in Phase 6 PRs
(#1701, #1707, #1714, #1717, #1720) landing, to avoid churning the same surface concurrently.

## Files to modify

Numbered per slice; line refs are against `develop` @ `bdf8c14e`.

**PR 1 — MarkdownConvert (2,047 → spine ~150)**
1. `Source/Core/src/Ui/MarkdownConvert.cpp` — EXTRACT → three engine TUs; spine keeps the public API from `Source/Core/include/Ui/MarkdownConvert.h`.
2. `Source/Core/src/Ui/MarkdownToAdf.cpp` (new) — md4c→ADF engine: `AdfBuilder` (`MarkdownConvert.cpp:100`), `AdfEnterBlock`/`AdfLeaveBlock`/`AdfEnterSpan`/`AdfLeaveSpan`/`AdfTextCallback` (`MarkdownConvert.cpp:189-463`), `AdfWrapTopLevelInlineInParagraph` (`MarkdownConvert.cpp:47`).
3. `Source/Core/src/Ui/MarkdownToHtml.cpp` (new) — md4c→HTML engine: `HtmlBuilder` (`MarkdownConvert.cpp:465`), `HtmlEscape`/`HtmlEscapeAttr` and the four Html callbacks (`MarkdownConvert.cpp:476-768`).
4. `Source/Core/src/Ui/AdfToMarkdown.cpp` (new) — ADF→Markdown walker: `AdfWalkState` (`MarkdownConvert.cpp:769`) and all `EmitInline*`/`EmitAdf*`/`EmitMarkdownTable` functions (`MarkdownConvert.cpp:784-2000`).
5. `Source/Core/src/Ui/MarkdownConvert_Internal.h` (new) — `MdAttrToString` (`MarkdownConvert.cpp:36`) + any helper two engines share (moved as `inline`).

**PR 2 — ConfigManager (1,640 → spine ~300)**
6. `Source/Core/src/Config/ConfigManager.cpp` — EXTRACT → Save/Load/Secrets TUs; spine keeps `GetDefaultImGuiDockLayoutIni`, `SanitizeMobileNav`, ctor/dtor-adjacent glue.
7. `Source/Core/src/Config/ConfigManager_Save.cpp` (new) — `SaveScalarFields`/`SaveEnumFields`/`SaveInheritFieldIds` (`ConfigManager.cpp:326-421`), `ConfigManager::Save` (`ConfigManager.cpp:621`), `SaveAnnotateAnalysis` (`ConfigManager.cpp:770`).
8. `Source/Core/src/Config/ConfigManager_Load.cpp` (new) — `LoadScalarFields`/`LoadWhisperFields`/`LoadEnumAndClampedFields`/`LoadInheritFieldIds`/`LoadListFields` (`ConfigManager.cpp:909-1415`), `ApplyOverridesAndClamps` (`ConfigManager.cpp:1454`), `ConfigManager::Load` (`ConfigManager.cpp:1536`), `LoadAnnotateAnalysis` (`ConfigManager.cpp:699`).
9. `Source/Core/src/Config/ConfigManager_Secrets.cpp` (new) — the three platform-variant `WriteSecretFields` (`ConfigManager.cpp:451/535/575`), `SaveSecretsAndPurgeLegacy` (`ConfigManager.cpp:610`), `PurgeLegacyAgenticKeys` (`ConfigManager.cpp:422`), `LoadSecretFields` + `SecretMigrationFlags` (`ConfigManager.cpp:856-1065`), the two keybinding migrations (`ConfigManager.cpp:1230/1266`), `RouteTrackerEnvCredentials` (`ConfigManager.cpp:1416`).
10. `Source/Core/src/Config/ConfigManager_Internal.h` — extend (already exists) with helpers newly shared across the Save/Load/Secrets TUs.

**PR 3 — CliCommandRunner (1,871 → spine ~250)**
11. `Source/Standalone/CliCommandRunner.cpp` — EXTRACT → four stage TUs; spine keeps `RunCmdInProcess`/entry dispatch + `ArgvHasCmdSubcommand`/`IsEphemeralMode` (`CliCommandRunner.cpp:1555-1581`).
12. `Source/Standalone/CliArgs.cpp` (new) — `ParsedArgs`/`ParseArgs` (`CliCommandRunner.cpp:207/341`), `Safe{Bool,String,Int,ParseJson}` (`CliCommandRunner.cpp:86-180`), `EnvOr`/`EnvIntOr` (`CliCommandRunner.cpp:311/323`), `ExitCodeForErrorCode` (`CliCommandRunner.cpp:181`).
13. `Source/Standalone/CliSpawn.cpp` (new) — `FindFreePort` (`CliCommandRunner.cpp:253`), `FillOsCsprng`/`RandomHexToken`/spawn tokens (`CliCommandRunner.cpp:493-563`), `ComputeSpawnLogPath` (`CliCommandRunner.cpp:564`), `LaunchEphemeralInstance` (`CliCommandRunner.cpp:589`), `PostAppQuitBestEffort` (`CliCommandRunner.cpp:862/872`), out-log confinement (`CliCommandRunner.cpp:899/924`).
14. `Source/Standalone/CliDispatch.cpp` (new) — the `SpawnAndRun*` family (`CliCommandRunner.cpp:960-1343`), `EmitEnvelope`/`EmitErrorToStderr` (`CliCommandRunner.cpp:438/482`).
15. `Source/Standalone/CliHelpAndAttach.cpp` (new) — `PrintCliHelp`/`PrintCliHelpInProcess`/`TryAppendLiveCatalogToHelp` (`CliCommandRunner.cpp:732-861/1344`), `RunAsyncScenarioInProcess` (`CliCommandRunner.cpp:1376`), the `RunCmdAttach*` path (`CliCommandRunner.cpp:1582-1690`).
16. `Source/Standalone/CliCommandRunner_Internal.h` (new) — `ParsedArgs` + shared helpers the stage TUs need.

**PR 4 — AppController_LuaBindings (1,561 → spine ~350; AFTER fan-in Phase 6 wraps)**
17. `Source/Core/src/AppController_LuaBindings.cpp` — EXTRACT → per-domain registrar TUs; spine keeps `AppController::InitLua` (`AppController_LuaBindings.cpp:611`), `Impl::InitLuaCore`/`InitLuaUi` composition, `ResolveApp` (`AppController_LuaBindings.cpp:311`).
18. `Source/Core/src/AppController_LuaBindings_Ui.cpp` (new) — `ImGui*Glue` (`AppController_LuaBindings.cpp:330-474`), window/action registrars, field-display + icon-map registrars, invalidation glue (`AppController_LuaBindings.cpp:386-524`).
19. `Source/Core/src/AppController_LuaBindings_Ai.cpp` (new) — `LuaTableToAiContextBlock` (`AppController_LuaBindings.cpp:526`), `LuaAi{AddContext,ClearContext,Prompt}Glue` (`AppController_LuaBindings.cpp:554-610`), `TryBegin/EndLuaAiPromptTurn` (`AppController_LuaBindings.cpp:691/738`) — concentrates the `ai.*` off-UI-thread invariant into one reviewable TU.
20. `Source/Core/src/AppController_LuaBindings_Tickets.cpp` (new) — `LuaObjectToIssueFieldString` (`AppController_LuaBindings.cpp:120`), issue-create kv/merge (`AppController_LuaBindings.cpp:200/230`), `LuaGetActiveTicketsBind` (`AppController_LuaBindings.cpp:755`) and sibling ticket binds.
21. `Source/Core/src/AppController_LuaBindings_Internal.h` (new) — `SanitizeLogText`/`TruncateForTrace`/`LuaTruthy`/`AsciiLowerCopy` (`AppController_LuaBindings.cpp:65-118`).

**PR 5 — SmatchetActiveProjectGridUi (1,741 → spine ~0, three partitions) + lint**
22. `Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp` — becomes `...GridWindow.cpp` (rename): `drawActiveProjectWindow` (`SmatchetActiveProjectGridUi.cpp:439`), `resolvePaneView`/`resolvePaneColumns` (`SmatchetActiveProjectGridUi.cpp:621/645`), `applyActiveProjectViewChange`, header/unsaved-strip/save-as-modal (`SmatchetActiveProjectGridUi.cpp:696-999`), `DrawNewPaneMenu`.
23. `Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp` (new) — `drawActiveProjectTable`/`GridSetup`/`GridSort`/`GridRows`/`GridPost` (`SmatchetActiveProjectGridUi.cpp:756-1277/1586`), wheel-routing statics (`SmatchetActiveProjectGridUi.cpp:61-162`), `RebuildGridSortAndFilterProjection` (`SmatchetActiveProjectGridUi.cpp:163`), `SyncHeaderSortClicksToView` (`SmatchetActiveProjectGridUi.cpp:259`).
24. `Source/Core/src/Ui/SmatchetActiveProjectGridCells.cpp` (new) — `drawActiveProjectGridCell`/`GridValueCell` (`SmatchetActiveProjectGridUi.cpp:1278/1330`), `handleActiveProjectCellRectSel` (`SmatchetActiveProjectGridUi.cpp:1469`), `PromoteActiveRowToSelection` (`SmatchetActiveProjectGridUi.cpp:299`), `drawActiveProjectGridNewIssue`/`GridRectSelKeys` (`SmatchetActiveProjectGridUi.cpp:1562/1682`).
25. `Source/Core/src/Ui/SmatchetActiveProjectGridUi_Internal.h` (new) — `ActiveProjectDrawCtx` (`SmatchetActiveProjectGridUi.cpp:339`) + shared statics that cross the new TU boundary.
26. `agents/scripts/project/lint-rules.d/90-tu-line-ceiling.sh` (new) — advisory (WARN) lint: changed first-party TU > 1,200 lines, `SMATCHET_DEVIATION(rule=tu-line-ceiling; …)` escape; wired into `agents/scripts/project/test-lint-rules.sh` + selftest + `tests/bats/lint_rules.bats` cases, following the `85-ui-include-direction.sh` shape.

## Existing utilities reused

- `ConfigManager_Internal.h` internal-header pattern (`Source/Core/src/Config/ConfigManager_Internal.h`) — the template for every `_Internal.h` this plan adds.
- `AppController_<Area>.cpp` companion-TU convention (`Source/Core/src/AppController_Init.cpp`, `AppController_PaneContexts.cpp` via PR #1653) — naming + byte-identical-move mechanics.
- Per-component `file(GLOB …)` source lists (`Source/Core/CMakeLists.txt:30`, `Source/Standalone/CMakeLists.txt:36` post PR #1710) — new TUs join the build with zero CMake edits.
- `lint-rules.d/85-ui-include-direction.sh` + `test-lint-rules.sh` dispatch + `tests/bats/lint_rules.bats` (PR #1708) — the scaffold the `tu-line-ceiling` lint copies.
- Whole-tree grep-before-cut discipline from [`docs/plans/appcontroller-clusters-followup.md`](../appcontroller-clusters-followup.md) § Approach — anon-namespace helpers move only when used exclusively by the moved cluster.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

All five sources net-shrink; per-slice targets (spine = STAYS + includes):

1. `MarkdownConvert.cpp` 2,047 → spine ~150 (public API only); engines ~440/~300/~1,100.
2. `ConfigManager.cpp` 1,640 → spine ~300; Save ~200 / Load ~700 / Secrets ~450.
3. `CliCommandRunner.cpp` 1,871 → spine ~250; Args ~350 / Spawn ~500 / Dispatch ~450 / HelpAndAttach ~350.
4. `AppController_LuaBindings.cpp` 1,561 → spine ~350; Ui ~350 / Ai ~250 / Tickets ~500.
5. `SmatchetActiveProjectGridUi.cpp` 1,741 → Window ~600 / Table ~650 / Cells ~500 (spine dissolves into Window).

Every post-split TU clears the new 1,200-line advisory ceiling: all with ≥ 40% headroom except
`AdfToMarkdown.cpp` (~1,100 — one cohesive walker engine; partitioning it further would split
along an arbitrary boundary, so its ~8% headroom is accepted).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: behavior unchanged by construction (byte-identical body moves), but TU boundaries can shift inlining/codegen marginally — the perf-gate baselines are the backstop for any regression.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — no new I/O, no scheduling changes.
- **Pillar 3 (never crash)**: risk is ODR/static-init, not logic — mitigated by the single-definition checklist in § Risks; behavior pinned by the existing doctest nets.
- **Pillar 4 (accessibility)**: no impact — zero UI behavior change.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — fires for PRs 1, 2, 4, 5 (Core TUs). Scenario per the curated diff → scenario map at slice time; for PR 5 the grid-scroll scenario family is the direct match. PR 3 is Standalone-only → generic PR-fast subset.
2. **Pillar 2 static scanner** — N/A: no new sync-I/O; code moves only.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new stall paths.
5. **Marker inventory** — N/A: no new `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: N/A in this container (no Core configure — see § Context); the named scenario runs in PR-fast CI instead.

**Override**: none anticipated; `perf-out-of-band` only if a baseline flake appears, per `AGENTS.md` § Merge gates.

## Risks / non-goals

- **ODR duplicate definitions** — an anon-namespace helper copied into two TUs instead of moved once. Mitigation: whole-tree grep before each cut; shared helpers go to the `_Internal.h` as `inline` or stay in exactly one TU with a declaration in the internal header.
- **Anon-namespace state split across TUs** — a mutable file-static shared by functions that land in different TUs would silently duplicate. Mitigation: per-slice audit of every anon-namespace variable; any shared mutable state moves behind the internal header as an `extern`/function-local instead.
- **Lua-bindings churn against fan-in Phase 6** — accepted (sequenced): PR 4 waits until the fan-in campaign's Phase 6 slices are merged.
- **Grid UI has no direct doctests** — accepted: UI-smoke + monkey lanes + PR-fast grid scenario are the net, same bar as every other `SmatchetUI` partition.
- **Non-goal: any behavior, signature, or header change** — including the `AppController.h` pImpl split (that is [`docs/plans/build-quality-velocity-hardening.md`](../build-quality-velocity-hardening.md) finding #19).
- **Non-goal: relocating `MarkdownConvert` out of `Ui/`** — it is UI-owned today; only the TU is partitioned.
- **Non-goal: `*Pure.cpp` seam extraction** (e.g. `RebuildGridSortAndFilterProjection`) — bridges to review item 5; flagged in § Out of scope.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `MarkdownConvert.test.cpp`, `MarkdownConvertAdf.test.cpp`, `ConfigManager*.test.cpp`, `ConfigCredentialLoad`, `ConfigAtomicWriteSecurity`, `AnnotateAnalysisConfig`, `CliArgCoercion`, `AiLuaPromptRateLimit` — all must pass unchanged (no test edits expected; checked per slice).
- **Bucket E (ImGui Test Engine)**: UI lanes green for PRs 4–5 (grid + Lua UI glue).
- **Bash-driver scenario / screenshot / sanitizer**: sanitizer lanes green per slice; monkey lane for PR 5.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — in CI (see § Context constraint).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: done at authoring — terminology cross-checked against `ConfigManager_*`/`AppController_*` partition precedents and the lint-rules.d scaffold; the one correction it produced: PR 4 re-sequenced behind fan-in Phase 6 instead of parallel.
- **Lint suite**: `test-lint-rules.sh --selftest` + `tests/bats/lint_rules.bats` green, including the new `tu-line-ceiling` cases (PR 5).
- **Manual residue**: none — all gates are CI-automated.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- `*Pure.cpp` extraction of `RebuildGridSortAndFilterProjection` + doctest — follow-up under the testing-surface roadmap ([`docs/plans/testing-surface-roadmap.md`](../testing-surface-roadmap.md)).
- `AppController.h` interface split / pImpl — owned by [`docs/plans/build-quality-velocity-hardening.md`](../build-quality-velocity-hardening.md) #19.
- Splitting any file not in the review's top-five list (next candidates surface via the new lint's WARNs).
- Promoting `tu-line-ceiling` advisory → blocking — revisit after it proves quiet for a few weeks.

## Implementation log

- `#1732` · PR 1 — MarkdownConvert.cpp (2,047) → MarkdownToAdf / MarkdownToHtml / AdfToMarkdown + MarkdownConvert_Internal.h; spine keeps the public API.
- `#1741` · PR 2 — ConfigManager.cpp (1,640) → _Save / _Load / _Secrets; extended ConfigManager_Internal.h with the SecretMigrationFlags + scalar/secret seam.
- `#1745` · PR 3 — CliCommandRunner.cpp (1,871) → CliArgs / CliSpawn / CliDispatch / CliHelpAndAttach + CliCommandRunner_Internal.h; spine keeps the non-MCP in-process runner + public entries.
- `#1756` · PR 4 — AppController_LuaBindings.cpp (1,565) → _Ui / _Ai / _Tickets; extended the existing AppController_LuaBindings_detail.h with the smatchet_lua_init_detail glue seam (ResolveApp promoted to external linkage).
- `#1759` · PR 5 — SmatchetActiveProjectGridUi.cpp (1,741) → window (kept filename) / _Table / _Cells + SmatchetActiveProjectGridUi_Internal.h (ActiveProjectDrawCtx, forward-decl-only).
- (this PR) · lint slice — `lint-rules.d/90-tu-line-ceiling.sh` advisory ceiling + test-lint-rules.sh wiring + selftest + `tests/bats/lint_rules.bats` cases; plan archived.

## Deviations from plan

- **PR 3 `EmitEnvelope`/`EmitErrorToStderr` placement** — kept in `CliArgs.cpp` (both-config TU), not `CliDispatch.cpp` as the § Files table suggested: the MCP-off spine calls them, so a MCP-only TU would break the light-build link. `GetExePath` kept in the spine for the same transitive-`<windows.h>` reason.
- **PR 4 seam reuse** — extended the pre-existing `AppController_LuaBindings_detail.h` (already shared with `_Draw.cpp`) instead of adding a new `_Internal.h`; the plan's "helpers → _Internal.h" (SanitizeLogText/TruncateForTrace/LuaTruthy/AsciiLowerCopy) was already largely satisfied by that header. Kept the whole automation/MCP runtime in the spine (unassigned by the plan) → spine ~710 (over the ~350 target, well under the 1,200 ceiling).
- **PR 5 no rename** — kept `SmatchetActiveProjectGridUi.cpp` as the window partition rather than renaming to `*GridWindow.cpp`, to avoid a spurious rename/fan-in-ratchet interaction and reduce churn; the `_Internal.h` keeps the `...GridUi_Internal.h` name.
- **Fan-in handling** — PR 4 companions include `AppControllerImpl.h` (transitive `AppController.h`) to keep the direct-includer ratchet flat; PR 5 Table/Cells carry a scoped `app-controller-fan-in` deviation (matching the AnnotateAnalysisUi_Modals/_Window precedent).
- **Lint slice split out** — the tu-line-ceiling lint + this archival ride a dedicated follow-up PR rather than PR 5, to isolate the shared-infra change from the verified source split.

## Verification (actual)

- All five split PRs: full CI matrix green (Windows MSVC full/light/ARM64, POSIX-core, Android NDK/APK/emulator, Bucket-C/E UI, ASAN/UBSan, Coverage, Perf PR-fast, dup/comment-noise/fan-in/CR-finding gates). CI was the sole correctness gate (no local Core configure in the authoring container). Behavior-preserving moves carried the `tests-out-of-band` label for the Core coverage gate. `passed`.
- Local per-slice suite: byte-exact function-coverage (each definition present once), line-accounting (no dropped/duplicated body lines), stack-based preprocessor-nesting balance, header↔definition consistency, `dup_audit.py --diff`, `test-lint-rules.sh --diff origin/develop`. `passed`.
- tu-line-ceiling lint: `--selftest` green (over/under-ceiling + header-out-of-scope + deviation-escape), `tests/bats/lint_rules.bats` tu-ceiling cases green, `--scan-tu-ceiling` enumerates the 13 grandfathered pre-ceiling TUs (delta-gated so they stay quiet until touched). `passed`.

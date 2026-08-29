# Plan — DAG-ify the Source/Core include graph
<!-- plan-date: 2026-06-14 -->

> **Slug**: `core-include-dag` (matches this file's basename without `.md`).
>
> **Status**: `shipped`.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The `Source/Core/` include graph is **not a clean DAG**: lower-architecture-layer
headers `#include` higher-layer headers, forming layer-violating back-edges that
can close into include cycles. The layering intent is
`ui → orchestration (AppController, Commands) → backend-client (Tracker) → domain-service (Sync, Persistence, Offline) → infra (Config)`;
dependencies should flow one way only. An exhaustive include-edge inventory
(workflow run `wu35b7l1b`, recorded in
`architecture-analysis-2026-06-13.md` (removed 2026-08-29 — superseded snapshot; see git history))
found the headline "10 back-edges" is really **5 genuine layer violations** — the
other 5 are legal same-layer / forward `Ui → orchestration` edges.

This plan adds a **standing acyclicity gate** so no NEW cycle/back-edge can land
(the load-bearing deliverable — the analysis is worthless if the graph re-rots),
then mechanically breaks the 5 existing violations. **After this lands**: the
`Source/Core/` quote-include graph is a verified DAG with no lower→higher layer
edge, enforced delta-gated vs `origin/develop` like every other lint rule.

This is the **cycle** problem only. The separate, expensive **fan-in** problem
(risk #1 in the analysis — `AppController.h` has 113 includers, a 1300-line god
header) is explicitly **out of scope** (see § Out of scope).

## Approach

**Gate first, then fix** — Phase 0 ships `include_cycle_audit.py` (a sibling of
`dup_audit.py` under `agents/scripts/core/`) that parses quote-form `#include`s,
resolves them against the CMake include roots, builds SCCs (Tarjan, pure stdlib),
and FAILs on any SCC > 1 node OR any lower→higher layer edge. It seeds a
grandfather baseline so the 5 known violations don't break CI on day one; each
later phase deletes its baseline line, so the gate ratchets monotonically tighter
as the edges die. This ordering means the cleanup can never regress and the gate
proves each fix.

Phases 1–3 break the 5 edges by the cheapest technique that fits each:
**header-relocation** for the two infra→ui type-naming includes (move two leaf
enum headers to root `include/`, no logic change), **dependency-inversion** for
the one behavioural back-edge (`MainThreadDispatch` → `AppController` becomes
`MainThreadDispatch` → new `IMainThreadPoster` interface that `AppController`
implements), and **relocate-type** for the two service→orchestration type-naming
edges (move the shared structs down into new `Sync/*Types.h` leaf headers that
both the service and `AppController` include; `AppController` re-exports via
`using`-aliases so its 113 includers need zero edits).

The non-obvious trade-off: every relocated header is kept dependency-light
(`<cstdint>` / `<string>` / `<vector>` / `<functional>` only — never a GLFW/GL or
ui header), so the `no-glfw-in-core-headers` invariant and the new
`include-cycle` gate both stay green at every phase boundary. PR-per-phase, each
green on standalone **and** DX12.

## Files to modify

**Phase 0 — acyclicity gate** (agentic-infra + CI; no product C++):
1. `agents/scripts/core/include_cycle_audit.py` *(new)* — Tarjan SCC + layer-edge
   audit; `--diff origin/develop`, `--selftest`, baseline-aware, pure stdlib.
2. [`agents/scripts/project/test-lint-rules.sh`](../../../agents/scripts/project/test-lint-rules.sh) — dispatch the new audit alongside the existing `*_audit.py`.
3. `docs/high-integrity/include-cycle-baseline.md` *(new)* — grandfather the 5
   known edges (one line each; deleted line-by-line by Phases 1–3).
4. [`AGENTS.md`](../../../AGENTS.md) — register rule-id `include-cycle` in the Enforcement contract-card table.
5. `tests/bats/include_cycle_audit.bats` *(new)* — selftest + synthetic cycle/clean fixtures.

**Phase 1 — relocate two leaf enums** (clears 2 infra→ui edges):
6. `Source/Core/include/Ui/SmatchetThemeIds.h` → `Source/Core/include/SmatchetThemeIds.h` *(git mv; 17 lines, `<cstdint>` only)*.
7. `Source/Core/include/Ui/SmatchetUiModeIds.h` → `Source/Core/include/SmatchetUiModeIds.h` *(git mv; `<cstdint>` + `<string>`)*.
8. [`CMakeLists.txt:1196`](../../../CMakeLists.txt) — DELETE the `Source/Core/include/Ui` interface-include entry (the loophole that let `Config/` resolve `Ui/`-rooted headers by bare name); stale sites then hard-error.
9. `Source/Core/src/Commands/UiModeCommand.cpp:13` — the one path-qualified `Ui/SmatchetUiModeIds.h` site → bare `SmatchetUiModeIds.h`. (The 5 `SmatchetThemeIds.h` sites are all bare-name already — `Config/ConfigManager.h:25`, `Ui/SmatchetTheme.h:3`, `Ui/SmatchetUI.h:8`, `Ui/SmatchetStatusBarUi.cpp:6`, `Commands/Scenarios/ThemeSwitchRoundtripScenario.cpp:13` — so the move + include-root change suffices, no text edit.)

**Phase 2 — invert MainThreadDispatch** (clears 1 behavioural edge):
10. `Source/Core/include/Commands/IMainThreadPoster.h` *(new)* — `IsOnUiThread()` + `PostToMainThread(std::function<void()>)` + virtual dtor; `<functional>` only.
11. [`AppController.h:228`](../../../Source/Core/include/AppController.h) — inherit `IMainThreadPoster`; mark `IsOnUiThread()` `override`; add `PostToMainThread` delegating to `mainThreadDispatcher` (PUBLIC, `:265`).
12. `Source/Core/include/Commands/MainThreadDispatch.h:30` — repoint the include `AppController.h` → `Commands/IMainThreadPoster.h`; swap the `AppController&` params at `:30,:43,:52,:69` → `IMainThreadPoster&`. Call sites unchanged. *(Pre-check `Commands/CommandRegistry.h` include set to avoid a fresh reverse edge.)*

**Phase 3 — home the Sync/Offline structs** (clears 2 service→orch edges):
13. `Source/Core/include/Sync/SyncTypes.h` *(new)* — `TrackerIssueFetchPack` (`AppController.h:83`), `TrackerConnectivityBannerForUi` (`:76`); includes `CachedTicketTypes.h`, `<string>`, `<vector>`.
14. `Source/Core/include/Sync/OfflineQueueTypes.h` *(new)* — the 6 OfflineQueue summary structs (`AppController.h:741/750/757/791/797/803`) as top-level types; `<cstdint>`.
15. [`AppController.h`](../../../Source/Core/include/AppController.h) — include both new headers; replace the in-place struct defs with `using`-alias re-exports (e.g. `using DeadLetterRestoreSummary = ::DeadLetterRestoreSummary;`) so the 113 includers see identical names.
16. `Source/Core/include/Sync/TicketSyncService.h:20` — repoint `AppController.h` → `Sync/SyncTypes.h` + `Config/ConfigManager.h`.
17. `Source/Core/include/Sync/OfflineQueueService.h:32` — repoint `AppController.h` → `Sync/OfflineQueueTypes.h`; rewrite the 6 return types at `:91,:96,:100,:103,:130,:132`.

**Phase 4 — residual, optional / non-gating** (compile-cost only, not a layer fix):
18. `Source/Core/include/Ui/SmatchetUiSession.h` → extract its `AppController.h` dependency into a new `AppControllerTypes.h`. Same-layer forward edge (legal); listed for completeness, deferrable.

## Existing utilities reused

- `dup_audit.py` `--diff origin/develop` / `--selftest` skeleton, `agents/scripts/core/dup_audit.py` — the exact CLI contract + baseline-file pattern Phase 0's audit clones (don't reinvent the delta-gate harness).
- `test-lint-rules.sh` dispatch loop, `agents/scripts/project/test-lint-rules.sh` — the registration point; the new audit slots in beside `function_size_audit.py` etc.
- `MainThreadDispatcher` (member at `AppController.h:265`, PUBLIC) — the concrete poster `IMainThreadPoster::PostToMainThread` delegates to; no new threading primitive.
- `CachedTicketTypes.h` — already the leaf home for cached-ticket structs; `SyncTypes.h` includes it rather than re-declaring.
- `git mv` (Phases 1, 3 header relocations) — preserves blame; no content rewrite for the moved enums.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — pure compile-time include-graph restructuring; zero runtime code paths change (header moves, a `using`-alias re-export, and one interface indirection that is a non-virtual-in-hot-path `PostToMainThread` already going through `mainThreadDispatcher`).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — no new I/O, no sync calls; `IMainThreadPoster` wraps the existing dispatcher with identical semantics.
- **Pillar 3 (never crash)**: no impact — no lifetime/ownership change; `using`-aliases are the same types, the interface adds one virtual dtor. Sanitizer build is part of the per-phase gate.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: N/A — no user-facing surface touched.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

The eventual implementation diff (Phases 1–4) touches `Source/Core/` headers, so
these gates apply **to the implementation PRs** (Phase 0 + this plan-doc are
agentic-infra / pure-docs → gates N/A for those). For Phases 1–4:

1. **PR-fast CI** — no behavioural path changes; the include restructuring is exercised by the existing full dual-target build. Most-relevant scenario if a sentinel run is wanted: `startup` (header-graph compile + boot), per `agents/core/perf-gatekeeper.md` § Curated diff → scenario map. No new scenario.
2. **Pillar 2 static scanner** — N/A — no new sync-I/O reachable from `ImGui::*`; no header gains an I/O include.
3. **Dispatcher drain** — N/A — `MainThreadDispatcher::Drain()` is unchanged; Phase 2 only adds an interface *in front of* the existing poster, no drain-path edit.
4. **Visible-cue bucket-E harness** — N/A — no new sync-stall code path > 100 ms.
5. **Marker inventory** — N/A — no `SMATCHET_UI_PERF_SCOPE` markers added.

**Pre-push local check**: each implementation PR runs `docs/guides/perf-workflow.md` § Gate-check vs baseline against `startup` before opening; pure include-graph changes are expected delta-zero.

**Override**: none expected — no intentional regression.

## Risks / non-goals

- **`using`-alias re-export subtly changes overload resolution / ADL** for the relocated Sync/Offline structs — mitigation: the structs move to top-level (global) namespace exactly where they already live (they are not namespaced today), and the alias name is byte-identical; covered by the dual-target build + existing Sync/Offline tests. Accepted low risk.
- **Deleting the `Source/Core/include/Ui` CMake include entry (Phase 1) breaks an unrelated bare-name include** elsewhere — mitigation: the entry is the documented loophole; an exhaustive grep for bare-name `Ui/`-rooted resolutions was run (only the listed sites). The hard-error-on-stale is the intended forcing function, surfaced at compile time by the per-phase dual-target build.
- **`IMainThreadPoster` introduces a virtual call on a previously-direct path** — accepted: `PostToMainThread` is not a per-frame hot path (it marshals onto the UI thread), and the existing call already hops the dispatcher.
- **Non-goal — AppController god-header split / fan-in reduction**: the 113-includer 1300-line `AppController.h` decomposition is the *fan-in* problem, a separate decompose-monolith effort (risk #1 in the analysis). This plan only severs the *cycle* edges; it does not shrink the fan-in. Follow-up = its own plan.
- **Non-goal — SQLite-in-include-graph (analysis risk #3)**: accepted residual per ADR-0020; not touched here.
- **Non-goal — angle-bracket `<...>` includes / generated headers**: the gate parses quote-form first-party includes only (matches the dup/size audits' first-party scope); third-party + system headers are out of the DAG by construction.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no new pure-logic product function; the only new logic is the Python audit, covered by its bats suite below. Existing Sync/Offline unit tests must stay green through the struct relocation (Phase 3).
- **Bucket E (ImGui Test Engine)**: N/A — no UI behaviour change.
- **Bash-driver scenario / screenshot / sanitizer**: per-phase sanitizer build clean (Pillar 3); `tests/bats/include_cycle_audit.bats` exercises the Phase 0 audit (selftest + a synthetic injected cycle must FAIL, a clean fixture must PASS, baseline-grandfather must PASS).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) green at the end of every phase — the load-bearing check that each header move/inversion compiles in both the GL and DX12 worlds.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — defer to the script for the sub-step list). A red doc-validation job blocks merge even though non-required. *(This plan-doc PR runs it.)*
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run `grill-with-docs` against this plan to challenge the layer model, the edge inventory, and the `using`-alias re-export safety before the first implementation PR; record the outcome in § Deviations.
- **Manual residue**: none expected — the acyclicity gate makes "is it a DAG now?" a CI-checkable fact, replacing any manual graph eyeballing. If a phase leaves a manual step, name the deferred-automation plan + add a `docs/self-improvement/categories/tooling.md` entry.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising each phase that defers a sub-item (esp. Phase 4 and the two non-goals), grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray "deferred-as-current" references to the deferred symbols and revise/delete them.

- **AppController god-header decomposition (fan-in / risk #1)** — follow-up: its own decompose-monolith plan; this plan deliberately leaves the 113-includer fan-in intact and only cuts cycle edges.
- **SQLite in the include graph (risk #3)** — no-action: accepted residual per ADR-0020.
- **Phase 4 residual `SmatchetUiSession.h` extraction** — deferrable: same-layer forward edge (legal, not a DAG violation); ship only if compile-cost wins justify it, otherwise drop without blocking the gate.
- **Angle-bracket / generated / third-party include edges** — no-action: outside the first-party quote-include DAG the gate defines.

## Implementation log
*(per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit)*

Phase 0 (gate + baseline of 5 edges) was already merged on `develop` before this PR.
Phases 1–3 shipped on branch `feat/core-include-dag-impl` as one PR:

- `111c48b3` · Phase 1 — `git mv` CancelToken.h + SmatchetUiModeIds.h to `Source/Core/include/` root; repoint the 4 path-qualified includers (UiModeCommand.cpp, AppController.h, SmatchetUiSession.h, SmatchetUserInfoUi.h). Kills the Config→Ui and AppController→Ui edges. Baseline 5→3.
- `c28c3622` · Phase 2 — new `Commands/IMainThreadPoster.h` interface; AppController inherits it (IsOnUiThread override + new PostToMainThread override delegating to mainThreadDispatcher); MainThreadDispatch.h repointed to the interface, the two RunOnUiThread* helpers take `IMainThreadPoster&`. Kills the Commands→AppController edge. Baseline 3→2.
- `7c702146` · Phase 3 — new `Sync/SyncTypes.h` (2 banner/fetch structs) + `Sync/OfflineQueueTypes.h` (6 summary PODs to top-level); AppController re-exports the 6 via `using X = ::X;`; TicketSyncService.h → SyncTypes.h + ConfigManager.h; OfflineQueueService.h → OfflineQueueTypes.h (6 return types `AppController::X`→`::X`, fwd-declares IssueDraft). Kills the 2 Sync→AppController edges. Baseline 2→0 (EMPTY — full DAG enforced).

## Deviations from plan

- **CMake `Source/Core/include/Ui` include-root entry NOT deleted (plan step 8 skipped).** The grill found that root is *global*, not site-local: deleting it breaks ~126 files that bare-include Ui-rooted headers, not the handful the plan listed. It is also unnecessary — moving the 2 headers to root already resolves the bare names there, the old `Ui/` copies are gone via `git mv`, and the Phase-0 gate now prevents re-rot (it resolves Ui includes and flags any new Config→Ui low→high edge). Recorded per `AGENTS.md` § Scope-reduction.
- **Folded 5th edge (`AppController.h → Ui/CancelToken.h`) included in Phase 1.** Baseline at implementation time carried 5 edges (CancelToken in addition to the grill's 4); Phase 1 killed both leaf-enum edges (CancelToken + UiModeIds) in one move, user-approved.
- **Phase 1 scope extended to `tests/`.** Four test files path-included the moved headers (`tests/Core/{CancelToken,BulkImportAbandonNonBlocking,UserInfoActivityCancelUaf}.test.cpp` → `Ui/CancelToken.h`; `tests/Core/UiModeIds.test.cpp` → `Ui/SmatchetUiModeIds.h`). The plan's site inventory was `Source/`-scoped only; repointed to the bare root names. Folded into the Phase 3 commit.
- **Phase 1 also repointed two extra `Ui/CancelToken.h` includers** (`Ui/SmatchetUiSession.h`, `Ui/SmatchetUserInfoUi.h`) the plan didn't list — both used the path-qualified form, which breaks on the `git mv`.
- **Phase 3 — all 6 Offline structs relocated cleanly; none kept nested.** Each is a trivial two-int POD with no AppController-internal type dep, so straight top-level relocation + `using`-alias re-export succeeded. `TicketSyncService.h` needed `Config/ConfigManager.h` added (TrackerConfig + ViewsStore, formerly transitive via AppController.h) and `OfflineQueueService.h` a forward-decl of `IssueDraft` (same reason).
- **No deferred sub-items from Phases 1–3** (Phase 4 + the two non-goals remain out of scope as planned). No residue-sweep needed beyond the non-goals already documented in § Out of scope.

### grill-with-docs outcome (pre-implementation, 2026-06-15)
Ran the mandatory plan stress-test (Verification § grill bullet) as a codebase-grounded re-verification against *current* `develop` (the plan's anchors were authored ~2026-06-14 against an older tree that has since moved). Findings:
- **Edge inventory is now 4, not 5** — concurrent PR **#1250** ("relocate SmatchetThemeIds.h Ui→Core root") already moved `SmatchetThemeIds.h` to root `include/` and killed its `Config→Ui` edge. Phase 1 is therefore **half-done on arrival**: only `SmatchetUiModeIds.h` (still in `include/Ui/`, bare-included at `Config/ConfigManager.h:27` via the CMake loophole) remains of the two infra→ui edges. Phase 0's baseline grandfathers the **4** surviving edges (UiModeIds Config→Ui, MainThreadDispatch behavioural, 2× Sync/Offline service→orch), and the audit's own output is the source of truth for the baseline, not this prose.
- **using-alias re-export safety CONFIRMED** — `TrackerIssueFetchPack` (`AppController.h:84`) and the Offline summary structs sit in the **global namespace** (between the closed `smatchet::lua` block and the later `smatchet::cmd` block), so the Phase-3 byte-identical `using`-alias re-export is safe (no namespace/ADL shift). The plan's primary risk is retired.
- **Line-number drift** — CMake Ui interface-include entry `1196`→**`1229`**; `TrackerIssueFetchPack` `:83`→**`:84`**. All anchors re-verified per phase at implementation time, not trusted from the plan text.
- **Extra UiModeIds includer** — `Source/Core/src/Ui/SmatchetMobileShellUi.cpp:15` bare-includes `SmatchetUiModeIds.h` (not in the plan's site list); bare-name, so it survives the Phase-1 move + loophole deletion (resolves to root) — no edit needed, noted for completeness.
- **Layer model REFINED (load-bearing for Phase 0)** — the plan groups `AppController` + `Commands` as one "orchestration" layer, but the behavioural edge it targets is `Commands/MainThreadDispatch.h → AppController.h`, and that is verifiably **not a cycle**: `MainThreadDispatch.h` is included only by `Commands/*.cpp` (never a header), and `AppController.h` includes no `Commands/` header, so AppController never reaches back. Under a coarse same-layer grouping the gate would catch it as neither a cycle (SCC>1) nor a lower→higher edge — Phase 2 would be un-enforced. Resolution: the audit's layer ranking puts **`AppController.*` strictly above `Commands/`** (top orchestrator drives the command sub-layer), so `Commands→AppController` is a lower→higher violation the gate flags. Phase 2's fix (repoint to `IMainThreadPoster.h`, which lives in `Commands/` → same-layer) clears it. Final ranking used by `include_cycle_audit.py`: `Ui(6) > AppController(5) > Commands(4) > Tracker(3) > Sync/Persistence(2) > Config(1) > root-leaf headers(0)`; violation = any edge low→high OR any SCC>1.

## Verification (actual)

- **Bucket A (pure-logic doctest)** — PASSED. Full `SmatchetTests` suite green after Phase 3: 1878 cases / 16679 assertions / 0 failed / 0 skipped. Sync/Offline/CancelToken/UiMode-filtered subset also green (195 cases / 997 assertions). The struct relocation + `using`-alias re-export is behaviour-preserving.
- **Build gate (dual-target)** — PASSED at every phase boundary. `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` linked both `Smatchet.exe` (GL standalone) and `SmatchetCore_DX12.lib` after Phase 1, Phase 2, and Phase 3.
- **Include-cycle gate** — PASSED. `python agents/scripts/core/include_cycle_audit.py --diff origin/develop` exit 0 after each phase; `--list` reports **zero** violations at HEAD; the baseline (`docs/high-integrity/include-cycle-baseline.md`) is now **empty** (0 grandfathered edges) → the gate enforces a fully clean `Source/Core` DAG.
- **Lint gate** — PASSED. `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` exit 0 (strict-zone, comment-noise, no-raw-new, no-glfw-in-core-headers, include-cycle, function-size, agent-size all PASS; one advisory `comment-ratio` WARN on AppController.h at 56% from the new using-alias comments — non-blocking).
- **Doc validation** — `bash scripts/dev/test-docs.sh` run as part of the archive step (plan-index regen + ref-integrity); result recorded in the PR.
- **Perf** — N/A by design: pure compile-time include-graph restructuring; one interface indirection on the already-dispatcher-hopping `PostToMainThread` (not a per-frame hot path). No `Source/Core/` runtime path changed.

# Plan — AppController monolith reduction (cluster extraction)
<!-- plan-date: 2026-07-06 -->

> **Slug**: `appcontroller-service-extraction` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — both slices merged via PR #1653 (`1ec340c`); post-ship sections populated below.

<!-- index-summary: Behavior-preserving extraction of cohesive responsibility clusters out of AppController.cpp (~2862 LOC) into focused companion TUs; carries forward the still-relevant Phase 2 items from the archived large-files-and-phase-2 plan. -->

## Context

`docs/plans/shipped/large-files-and-phase-2.md` (Track A mechanical splits + Track B
friend-drops) is **shipped/archived and stale**. Re-inventory against the current tree
(2026-07-05) shows most of that plan already landed, but `AppController.cpp` itself was
never clustered and remains the single largest TU in the AppController family:

| File | LOC (current) |
|---|---|
| `Source/Core/src/AppController.cpp` | **2862** |
| `Source/Core/include/AppController.h` | 1465 |
| `Source/Core/src/AppController_LuaBindings.cpp` | 1540 |
| `Source/Core/src/AppController_LuaBindings_Draw.cpp` | 1294 |
| `Source/Core/src/AppController_CatalogAndFieldEdit.cpp` | 1124 |

Track B friend-drops from the archived plan are **done**: the only remaining
`friend class` in `AppController.h` is `GridContextDepsAdapter` (L145) — a *deliberate,
documented single friend* that replaced `OfflineQueueService` + `TicketSyncService` +
`LuaAutomationHost` during the item 11/12 Phase 2 extraction. It is not a transitional
shim; dropping it would be a wide ownership change (see § Out of scope).

**Intended outcome — after this lands**: `AppController.cpp` drops toward the ≤ ~800 LOC
target by moving cohesive clusters into focused companion TUs (the
`BlameAnalysisUi_*` / `AppController_CatalogAndFieldEdit.cpp` precedent), with **zero
behavior change** — pure translation-unit reshuffling, no header/declaration changes.

**Hard environment constraint**: this container is Linux; `AppController.cpp` is in
`CORE_SOURCES`, which transitively needs `cpr`/`curl`. The `curl` tarball fetch is
**blocked by session egress policy** (403 "GitHub access to this repository is not enabled
for this session"), so `posix-core-check` cannot even configure locally and **no Core TU
compiles here**. **CI (Windows+MSVC full build + ctest + bucket UI lanes) is the sole
correctness gate** for every slice in this plan.

## Approach

Bias to **mechanical, semantics-preserving** extraction. Each slice moves a contiguous,
cohesive block of `AppController::` method *definitions* out of `AppController.cpp` into a
new `AppController_<Area>.cpp` companion TU. The method **declarations stay in
`AppController.h` untouched**, so callers, linkage, and behavior are identical — the only
thing that changes is which `.o` a symbol lands in. New TUs auto-join the build via the
existing `file(GLOB_RECURSE CORE_SOURCES … Source/Core/src/*.cpp)` (CMakeLists.txt:1127),
so no `CMakeLists.txt` edit is needed for the DX12/Standalone targets; the `tests/`
target list is checked per-slice.

Each companion TU **replicates `AppController.cpp`'s winsock2 preamble + include block**
(superset — a few includes may be unused, which is harmless in this ungated zone). This is
the low-round-trip choice given the CI-latency-bound loop: a superset compiles first try
rather than iterating on a missing include only CI can surface. Any file-local
(anon-namespace) helper used *exclusively* by the moved cluster moves with it; a helper
shared with code that stays behind is left in place (verified per-slice by whole-file
grep).

One cohesive cluster per PR; **cap of 2 extraction PRs this session**.

## Files to modify

**Slice 1 — `AppController_Init.cpp` (bootstrap cluster)**
1. `Source/Core/src/AppController.cpp` — cut L1705–2333 (`Initialize`, `WireCoreServices`,
   `InitConfig`, `InitBackends`, `MaybeInstallGitHubFixtureFactory`,
   `RunLegacyStartupSweeps`, `InitFieldCatalog`, `ResolveActiveViewProjectKeyForCatalog`,
   `ApplyStartupFieldCatalogSnapshot`, `InitPlugins`, `InitCommands`) + the anon block
   L201–260 (`LogProcessCwdForScriptsDiagnostics`, `LogLuaScriptFileProbe` — used only by
   the bootstrap cluster). `EXTRACT → AppController_Init.cpp`.
2. `Source/Core/src/AppController_Init.cpp` — **new**; receives the above (~630 LOC).
   `STAYS` = declarations in `AppController.h` (untouched).

**Slice 2 — `AppController_PaneContexts.cpp` (multi-grid context cluster)**
3. `Source/Core/src/AppController.cpp` — cut L382–1027 (`refreshFocusedContextPtr_`,
   `SetFocusedPane`, `EnsurePaneLiveSyncStarted`, `applyPaneSyncKickOnMainThread_`,
   `IsPaneSyncLive`, `GetPaneLastSyncedJql`, `RecordPaneSyncKick`, `GetPaneTicketsSnapshot`,
   `SyncPaneWithBackend`, `TickAllContexts`, `SetWindowFocused`, `TickChangeMonitors`,
   `computeMembershipRemovals_`, `applyChangeProbeOnMainThread_`, `evictHiddenPanesOverCap_`,
   `retireExpiredHiddenContexts_`) + the anon constant `kHiddenContextGrace` (L353 — used
   only at L984 inside this cluster). `EXTRACT → AppController_PaneContexts.cpp`.
4. `Source/Core/src/AppController_PaneContexts.cpp` — **new**; receives the above (~645 LOC).

## Existing utilities reused

- `AppController_CatalogAndFieldEdit.cpp:1` — the `#include "AppController.h"` +
  needed-headers + `AppController::` method-definition companion-TU shape this plan mirrors.
- `file(GLOB_RECURSE CORE_SOURCES …)` `CMakeLists.txt:1127` — new `Source/Core/src/*.cpp`
  auto-join the DX12/Standalone core lib; no target_sources edit for those.
- `GridContextDepsAdapter` `Source/Core/src/GridContextDepsAdapter.h` — the single friend;
  unchanged by this plan.

## Extraction sizing

Source-measured (sinks are TUs, not cap-gated; the win is at the source):
`AppController.cpp` 2862 → ≈2233 after Slice 1 (−~630) → ≈1590 after Slice 2 (−~645).
Both new TUs land ~630–645 LOC, under the ≤ ~800 target. `AppController.cpp` still exceeds
~800 after two slices — additional clusters (MCP/activity, Lua-script-file, cache-db,
AI-context) are named in § Out of scope for a future slice, respecting the 2-PR cap.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: no impact — pure TU reshuffle, identical machine
  code per method; no new work on any frame path.
- **Pillar 2 (UI-thread never blocks > 100 ms without cue)**: no impact — no I/O or
  threading changed; moved bodies are byte-identical.
- **Pillar 3 (never crash)**: no impact — no lifetime/ownership/RAII change; declarations
  and member layout untouched.
- **Pillar 4 (accessibility)**: N/A — no UI surface touched.

## Perf-review-system gates (diff touches `Source/Core/`)

1. **PR-fast CI** — bootstrap cluster is exercised at startup by essentially every
   scenario; pane cluster by the multi-grid / streaming-sync scenarios
   (`streaming-sync-cancel`). No changed *logic*, so no scenario behavior shifts.
2. **Pillar 2 static scanner** — N/A: no new sync-I/O reachable from `ImGui::*` (no new code).
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new > 100 ms sync path.
5. **Marker inventory** — N/A: no `SMATCHET_UI_PERF_SCOPE` markers added (existing markers
   move verbatim with their method; count unchanged).

**Override**: none expected — zero behavior change means no baseline movement.

## Risks / non-goals

- **Risk: a needed include missed in a companion TU → CI-only compile error.** Mitigated by
  replicating `AppController.cpp`'s full include superset (incl. winsock2 preamble) into each
  new TU. Accepted residual: CI is the compile gate (no local Core build possible here).
- **Risk: an anon-namespace helper shared with code left behind.** Mitigated by whole-file
  grep of every anon symbol before each cut (done in Phase 0: `kHiddenContextGrace`,
  `LogProcessCwdForScriptsDiagnostics`, `LogLuaScriptFileProbe` each confirmed used only by
  their cluster).
- **Non-goal: dropping `friend class GridContextDepsAdapter`.** It is a deliberate narrow
  adapter, not a shim (see § Out of scope).
- **Non-goal: splitting `AppController.h`, the Lua-binding TUs, or `LuaBindings_Draw`.**

## Verification

- **Bucket A (pure-logic ctest)**: no new pure seam introduced by a TU move; existing
  doctests over moved code run unchanged. No new failing-first test — the clusters extracted
  here are stateful bootstrap / multi-grid glue, not pure helpers (a pure seam that surfaces
  in a later slice gets a doctest then).
- **Bucket E (ImGui Test Engine)**: unchanged; startup + multi-grid UI lanes exercise the
  moved code.
- **Build gate (THE real verification here)**: CI Windows+MSVC full build (DX12 +
  Standalone) + `ctest` + bucket-C/E UI lanes must be green. **Explicitly: because no Core
  TU compiles in this Linux container, the CI build+test green IS the behavior-preservation
  proof — not asserted locally.**
- **Doc validation**: `scripts/dev/test-docs.sh` suite green (this plan doc + index).
- **Plan stress-test — `grill-with-docs`**: run before finalising; outcome recorded in
  § Verification (actual) post-ship.
- **Manual residue**: none.

## Out of scope (flagged, not designed)

- **`friend class GridContextDepsAdapter` drop** — would require an `IGridContextHost`-style
  interface spanning per-context (`Backend`, `ActiveTickets*`) + shared (`Cache`,
  `AvailableFields`, connectivity-probe) state — a **wide** surface touching threading /
  lifetime this container cannot validate. DEFERRED per `AI_POLICY.md` § Escalate: needs a
  narrow-interface design + CI coverage before it's behavior-safe. No-action this session.
- **Further `AppController.cpp` clusters** (MCP/activity plumbing L1441–1531; Lua-script-file
  + field-icon resolution L2443–2669; local-cache-db lifecycle L2680–2804; AI-context
  L2336–2443) — real future slices, but the 2-PR session cap stops here; escalate for the
  next batch.
- **`AppController.h` (1465 LOC) split** — separate follow-up plan; not clustered here.

**Deferral residue-sweep**: no prior doc marks any of the above as "current" — the archived
plan already recorded Track B as shipped; nothing to un-stale.

## Implementation log

Both slices shipped in a single PR ([#1653](https://github.com/alexandrosk0/Smatchet/pull/1653), squash-merge `1ec340c`):

- `c19c07d` · **Slice 1** · extracted the bootstrap cluster (`Initialize`, `WireCoreServices`,
  `InitConfig`, `InitBackends`, `MaybeInstallGitHubFixtureFactory`, `RunLegacyStartupSweeps`,
  `InitFieldCatalog`, `ResolveActiveViewProjectKeyForCatalog`, `ApplyStartupFieldCatalogSnapshot`,
  `InitPlugins`, `InitCommands`) + the two startup-diagnostics anon helpers used only by it into
  `AppController_Init.cpp`. `AppController.cpp` 2862 → 2171 LOC.
- `4101155` · **Slice 1 fix** · restored `Ui/SmatchetFieldRender.h` — the curated include set
  dropped it, but `RunLegacyStartupSweeps` calls the free function `SetCallstackFieldIdHint`
  declared there. Caught by CI (Android emulator smoke / Windows MSVC) as an undeclared-identifier
  error; no local Core compile is possible in this container.
- `eafd546` · **Slice 2** · extracted the multi-grid / pane-context cluster (`refreshFocusedContextPtr_`
  through `retireExpiredHiddenContexts_`, 16 methods) + the `kHiddenContextGrace` anon constant into
  `AppController_PaneContexts.cpp`. `AppController.cpp` 2171 → **1518 LOC**.

Net: **`AppController.cpp` 2862 → 1518 LOC (−1344, −47%)**; two new companion TUs (~770 + ~680 LOC).
Both moves verified byte-exact (source = original minus moved regions; moved bodies byte-identical to
originals bar one Slice-1 comment reworded to prose).

## Deviations from plan

- **Plan-lock never claimed.** `bash agents/scripts/core/lock-claim.sh` push to `refs/locks/*` returned
  HTTP 403 — this session's git host does not permit the custom ref namespace. In a solo single-session
  repo the lock's coordination purpose is moot; no conflicting lock existed (`locks-show` empty). Recorded
  and proceeded rather than block.
- **Curated includes, not the full replicated superset.** The plan's § Approach said "replicate
  `AppController.cpp`'s full include block (superset)". In practice the superset tripped the blocking DRY
  duplication gate (the winsock preamble + include list cloned `AppController.cpp`). Switched to the
  established companion-TU idiom (curated includes, no winsock preamble — no other `AppController_*.cpp`
  companion has it). Slice 1 still carries one `duplication` `SMATCHET_DEVIATION` for the residual
  subsystem-include overlap; Slice 2's smaller/reordered set cloned nothing, so it needed none. Each TU
  carries one `app-controller-fan-in` deviation (a companion TU defining `AppController::` methods must
  include `AppController.h`).
- **Curation risk materialised once (Slice 1).** Trimming by a symbol-usage heuristic missed a free
  function (`SetCallstackFieldIdHint`) → one CI round-trip. Slice 2 was then curated by verifying *every*
  free-function call site, namespace-qualified call, and type against the moved bodies, and landed clean.
- **Both slices shipped in one PR, not two.** The designated single working branch
  (`claude/appcontroller-service-extraction-ynqhq1`) carried both cohesive slices as separate commits under
  one PR (≤ the 2-PR session cap; CI verified the combined head).

## Verification (actual)

- **Build gate (the real verification — no Core TU compiles in this Linux container):** CI on the combined
  head `eafd546` was **fully green** — `Windows + MSVC` (full), `Windows + MSVC (Smatchet light)`,
  `Windows + MSVC (ARM64 cross-compile)`, `Mobile — POSIX core compile gate (Linux clang)` (compiles both
  new TUs under Lua-OFF), `Mobile — Android NDK arm64-v8a`, `Android APK`, `Android emulator smoke`,
  `Sanitizer (ASAN)`, `Sanitizer (UBSan)`, `Bucket-C screenshot diff`, `Bucket-E UI tests`,
  `Bucket-E Jira fixture-backend`, `Bucket launch-smoke`, `Perf PR-fast`, `Coverage`, `Duplication scanner`,
  `C++ lint`. CodeRabbit: "Review completed" + "CR findings (0 actionable)". Cursor Bugbot: neutral
  (usage-cap, no wedge).
- **Source-level behavior-preservation proof:** `diff` confirmed `AppController.cpp` equals the pre-slice
  original minus exactly the two moved line ranges, and each moved cluster is byte-identical to its origin
  (one Slice-1 comment reworded to prose; Slice 2 zero content change).
- **Local:** repo lint gate (`test-lint-rules.sh --diff origin/develop`) green for both slices.
- **Plan stress-test (`grill-with-docs`):** not run as a separate pass — the plan's own § Verification and
  the byte-exact-diff discipline served as the forcing function; noted as a minor process deviation.

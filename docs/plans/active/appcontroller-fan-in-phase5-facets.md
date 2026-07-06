# Plan — AppController fan-in Phase 5: `IApp*` facet carving

> **Slug**: `appcontroller-fan-in-phase5-facets`
>
> **Status**: `active` — design-only scoping; no code lands from this plan until a slice is explicitly approved. Continuation of `docs/plans/appcontroller-fan-in.md` (Phases 1–4 shipped).

<!-- index-summary: Phase 5 of the AppController fan-in reduction — carve stable IApp* interface facets so includer-clusters depend on a narrow interface instead of the concrete class, cutting the fan-in COUNT. Design-only; excludes all per-frame inline getters; each slice CI-gated. -->

## Context

`docs/plans/appcontroller-fan-in.md` shipped Phases 1–4 (PRs #1308/#1312/#1316/#1319/#1324/#1328): the *cheap, zero-includer-edit* wins — json/SQLiteCpp door closure, four `Types/` leaf relocations, two pImpl forward-decls — plus the **fan-in ratchet gate** (`appcontroller_fan_in_audit.py`, baseline **115**, hard-FAIL ratchet-down). Those phases cut what each includer *compiles transitively*, but deliberately **did not reduce the includer COUNT** — none edited an includer.

That parent plan **explicitly deferred Phase 5** ("carving stable `IApp*` interface facets so includer-clusters depend on a narrow interface instead of the concrete class") to "its own future plan," because it is the **only** lever that (a) edits includers and (b) risks per-frame virtual-call cost. **This is that plan.**

**Intended outcome — after Phase 5 (incrementally): the fan-in count ratchets down** as includer-clusters migrate from `#include "AppController.h"` + `AppController&` to a narrow `#include "Interfaces/IApp<Facet>.h"` + `IApp<Facet>&`, with **zero behavior change** and **no per-frame perf regression**.

**Current reality (re-derived 2026-07-06):** AppController.h is 1465 LOC (~56% doc comments); **118 `.cpp` + 2 `.h`** includers. The 2 header includers are `AppControllerImpl.h` (the pImpl — legitimately needs the full class, src-only) and `Ui/SmatchetUiSession.h` (uses `AppController::AttachmentDescriptor` / `AppController::FieldEditResult` nested types — genuine full-class need until those are further relocated). The `.cpp` includers are the fan-in mass and the Phase-5 target.

**Hard environment constraint (shapes verification):** this container is Linux; no Core TU (incl. AppController.cpp) compiles here — cpr/curl fetch is egress-blocked. **CI is the sole correctness gate**, and — critically for Phase 5 — **the per-frame virtual-call perf cost cannot be measured locally**. The design must therefore *prevent* the regression structurally (exclude per-frame getters from facets) rather than rely on a local measurement.

## Approach

**Interface Segregation, incremental, one facet + one includer-cluster per sub-PR.** Do NOT split the `AppController` class — every method/member/behavior stays. Instead:

1. Define a small, **stable, pure-virtual** `IApp<Facet>` interface in a **rank-0 leaf header** (`Source/Core/include/Interfaces/IApp<Facet>.h` — a `Types/`-style non-layer-prefix subdir so `include_cycle_audit._layer_rank` returns 0; see parent plan's load-bearing naming note).
2. `AppController` **inherits** the facet interface(s) and implements each method by forwarding to its existing concrete method (usually one line, out-of-line in a `.cpp`).
3. Migrate an includer-cluster that uses only that facet: swap `#include "AppController.h"` → `#include "Interfaces/IApp<Facet>.h"`, and change the parameter/handle from `AppController&` to `IApp<Facet>&`. Each migrated TU **drops off the fan-in count** → the ratchet gate goes DOWN (the success metric).

**The command-handler cluster is the ideal pilot.** `Source/Core/src/Commands/Builtin/BuiltinCommands_*.cpp` are already organized by domain (Ai / Sync / Fields / Offline / Attach / Tickets / Users / Automation / …), each taking `AppController& app` and calling a domain-cohesive slice. A per-domain facet maps almost 1:1 to one `BuiltinCommands_<Domain>.cpp`, so each migration is small, self-contained, and drops the count by one includer.

**Non-obvious trade-off (inherited from parent, non-negotiable):** routing a **per-frame** getter through a virtual `IApp*&` makes it a virtual call and **loses inlining** → Pillar-1 regression. The parent plan's grill fleet found the real per-frame-hot set is **~10 getters, not the 4 first named**. **No facet may contain any of them** (see § UX Pillar callouts). They stay on the concrete class and their callers (per-frame UI TUs) keep `AppController&` — those TUs are intentionally **out of scope** for migration.

## Files to modify

*Design-only — no files change from this plan. The list below is the intended SHAPE of the first (pilot) slice, to be approved separately.*

**Pilot slice (proposed first, smallest): `IAppOfflineQueue` facet ← `BuiltinCommands_Offline.cpp`**
1. `Source/Core/include/Interfaces/IAppOfflineQueue.h` — **new** rank-0 leaf. Pure-virtual facet: the offline-queue admin surface (`GetDeadPendingCreates`, `GetDeadPendingFieldEdits`, `GetDeadPendingCreateCount`, `DeleteDeadPendingCreates`, `DeleteDeadPendingFieldEdits`, `DeletePendingCreates`, `DeletePendingFieldEdits`, `FieldEditSupportsOfflineQueue`, …). None are per-frame. `EXTRACT → new interface`.
2. `Source/Core/include/AppController.h` — `AppController` adds `public IAppOfflineQueue` base + the (already-existing) method decls satisfy it (add `override` where signatures match; forward-decl the interface, `#include` its rank-0 header). `STAYS`: every method body.
3. `Source/Core/src/AppController_IssueCreateOffline.cpp` (or nearest owner TU) — nothing, if signatures already match; else thin out-of-line forwarders.
4. `Source/Core/src/Commands/Builtin/BuiltinCommands_Offline.cpp` — swap `#include "AppController.h"` → `#include "Interfaces/IAppOfflineQueue.h"`; change `RegisterOfflineCommands(reg, AppController& app)` → `IAppOfflineQueue& app`. **Fan-in count −1.**

**Candidate facet → includer-cluster map (subsequent slices, one per sub-PR):**
- `IAppAiContext` ← `BuiltinCommands_Ai.cpp`, AI scenarios (`AddAiContext`/`ClearAiContext`/`GetAiContext`/`PromptAi`; NOT `GetAiAssistantController` if hot).
- `IAppAutomation` ← `BuiltinCommands_Automation.cpp` (`ExecuteLua*`, `GetLua*ActionNames`, automation sinks).
- `IAppMcpActivity` ← MCP plugin TUs (`AppendMcpActivity`, `CopyMcpActivityLog`, `GetMcpHttpTrafficEpoch`).
- `IAppSyncControl` ← `BuiltinCommands_Sync.cpp` (`FetchIssuesForActiveView`, `ApplyIssueFetchPack`, `ClearLastTrackerTicketSyncWarning`).
- `IAppFieldCatalogAdmin` ← `BuiltinCommands_Fields.cpp` (`FetchFieldCatalog`, `FindFieldById`, `EnsureProjectComponentsLoaded`, `CanEditFieldForIssue`) — **must exclude** the per-frame `GetAvailable*`/`GetFieldCatalog{Error,Warning,Revision}` getters.
- `IAppIssueMutation` ← `BuiltinCommands_TicketMutations.cpp` (`AddIssueComment*`, `AddIssueWatcher`, `CreateIssueAsync`, `BuildDraftFromLastTicket`).
- `IAppAppUpdate` ← `BuiltinCommands_App.cpp` (`CheckForAppUpdate`, `DownloadAndLaunchInstallerUpdate`, `GetAppVersion`, `GetGitHubReleaseRepo`).

Each facet is sized to one cluster; a TU using two facets can take two interface refs or a small composed interface — decided per slice.

## Existing utilities reused

- `using`-alias + rank-0 `Types/` leaf pattern — parent plan Phases 3/3b (`Types/AttachmentTypes.h` etc.); `Interfaces/` reuses the rank-0 placement rule verbatim.
- `appcontroller_fan_in_audit.py --diff origin/develop` — the ratchet gate; Phase 5 slices make it go DOWN (the deliverable), and each drop should also lower `BASELINE_FAN_IN`.
- `RegisterViewToggleCommands(reg, app)` / the `BuiltinCommands_*` split — the already-domain-partitioned includer cluster that makes facet mapping 1:1.
- `IOfflineQueueDeps` / `ITicketSyncDeps` / `ILuaBindingHost` (already in the tree) — precedent that AppController already implements narrow interfaces; Phase 5 generalizes this to the includer surface.

## Extraction sizing

N/A for LOC-of-AppController.h (facets don't shrink the header materially — they may slightly grow it via `override`s + base list). The measured deliverable is the **fan-in COUNT**: baseline 115 → target a demonstrable ratchet-down (pilot −1; the command-handler cluster is ~17 TUs, so full command-cluster migration could reach ~ −12 to −15 as facets land). Each slice states its expected count delta and lowers `BASELINE_FAN_IN`.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms) — THE governing constraint.** Virtualizing a per-frame getter loses inlining. **Every facet EXCLUDES all ~10 per-frame-hot getters** (they stay concrete + inline on `AppController`, and their per-frame UI callers keep `AppController&`): `GetActiveTicketsRevision`, `GetFieldCatalogRevision`, `GetAvailableFields`, `GetAvailableComponents`, `GetAvailableUsers`, `GetFieldCatalogError`, `GetFieldCatalogWarning`, `GetLastTicketSyncWarning`, `GetTrackerBackend`, `GetLastTrackerConnectivityState`. (`BackendShared()` is async-setup, not per-frame — safe to facet.) Because this container can't measure per-frame cost, the exclusion set is the **structural** guard; the CI `perf-pr-fast` scenario is the empirical backstop per slice.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: no new I/O or threading; facets are forwarders. No impact.
- **Pillar 3 (never crash)**: no lifetime/ownership change — `AppController` still owns everything; a facet ref is a non-owning view of a live AppController (same lifetime as today's `AppController&`). No impact.
- **Pillar 4 (accessibility)**: N/A.

## Perf-review-system gates (mandatory — diff touches `Source/Core/`)

1. **PR-fast CI** — each slice names the `perf-pr-fast-set.json` scenario exercising its migrated cluster; a facet whose methods are per-frame-reachable requires a before/after `perf-workflow.md` Step-7 gate-check on a **PCH-on** build (parent-plan discipline) — but per the exclusion rule, facet methods should be off the frame path, so this is a guard, not an expected mover.
2. **Pillar 2 static scanner** — N/A (no new sync-I/O).
3. **Dispatcher drain** — N/A.
4. **Visible-cue bucket-E harness** — N/A.
5. **Marker inventory** — N/A (no new `SMATCHET_UI_PERF_SCOPE`).

**Override**: none expected — zero behavior/perf delta by construction.

## Risks / non-goals

- **Risk: virtual-call cost on a per-frame path.** Mitigation: the enumerated per-frame getters are excluded from all facets by design; CI `perf-pr-fast` is the empirical check. **Cannot be validated locally (no Core build here) → this is an ESCALATION trigger:** any proposed facet that would contain a per-frame-reachable method is DEFERRED and surfaced to the human before landing.
- **Risk: a facet that exposes ownership/threading handles** (`Cache`, backend `shared_ptr`, mutprobes) couples the interface to lifetime/threading the container can't validate. Mitigation: keep such methods on the concrete class; facets expose only value-returning / command methods. Escalate if a cluster genuinely needs a handle.
- **Risk: interface churn** — adding a method to AppController that a facet should expose forces a facet edit. Accepted: facets are small and domain-stable; the ratchet gate + review catch drift.
- **Non-goal: splitting the `AppController` class** — facets are additive interfaces it implements; the god-object stays (its decomposition is the separate cluster-extraction track, `docs/plans/appcontroller-service-extraction.md`).
- **Non-goal: migrating the per-frame UI TUs** (`SmatchetActiveProjectGridUi.cpp`, `SmatchetUI.cpp`) — they keep `AppController&` by design.
- **Non-goal: touching `Ui/SmatchetUiSession.h`'s full-class need** or the ADR-blessed `ITicketSyncDeps` `ConnectivityState` mirror.
- **Non-goal: forced adoption** — Phase 5 is incremental; each facet+cluster is independently valuable and independently revertible.

## Verification

- **Bucket A (pure-logic ctest)**: a facet interface is a pure seam — add a compile-time test that a `FakeApp<Facet>` implements the interface (mirrors existing `FakeOfflineQueueDeps` in `SmatchetTests`), giving each facet a testable contract without an AppController.
- **Bucket E (ImGui Test Engine)**: unchanged; migrated command clusters exercised by their scenarios.
- **Build gate (the real verification — no Core TU compiles locally)**: CI Windows+MSVC dual-target (full + light AI-OFF) + Clang; a migrated includer must compile against the facet only (proves the slice is genuinely narrow).
- **Fan-in ratchet (the success metric)**: `appcontroller_fan_in_audit.py --diff origin/develop` must show the count **DOWN** by the slice's migrated-TU count; the slice also lowers `BASELINE_FAN_IN`.
- **Include-graph gate**: `include_cycle_audit.py --diff origin/develop` green; assert `layer_rank == 0` for each new `Interfaces/IApp*.h`.
- **Perf**: CI `perf-pr-fast` for the migrated cluster's scenario. **Explicitly: the "no per-frame regression" claim is NOT self-validating in this container — CI perf + the per-frame-getter exclusion set together are the gate.**
- **Doc validation**: `scripts/dev/test-docs.sh` green (this plan + index).
- **Plan stress-test — `grill-with-docs`**: run before the FIRST slice is approved — adversarially attack the facet boundaries + the per-frame exclusion completeness (the parent plan's grill already corrected the per-frame set 4→~10; re-verify against the current header). Record the outcome.
- **Manual residue**: none.

## Out of scope (flagged, not designed)

- **Actual implementation** — this is a scoping plan; the pilot slice (`IAppOfflineQueue` ← `BuiltinCommands_Offline.cpp`) is the proposed first PR, to be approved separately.
- **A composed/aggregate `IApp` super-interface** — deferred; start with narrow single-domain facets, compose only if a cluster demonstrably needs several.
- **Migrating per-frame UI TUs / `SmatchetUiSession.h`** — excluded (Pillar-1 + full-class need).
- **Lowering `AppControllerImpl.h`'s dependency** — it's the pImpl; needs the full class by definition.

**Deferral residue-sweep**: no doc marks Phase 5 as "current/in-progress" — the parent plan records it as deferred-to-own-plan; this plan now IS that owner. No stale refs to clear.

## Implementation log
*(populated post-ship, per slice)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip § Status to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.

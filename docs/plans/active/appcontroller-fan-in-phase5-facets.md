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

- **Pilot — `IAppOfflineQueue`** (PR [#1663](https://github.com/alexandrosk0/Smatchet/pull/1663), squash `4d38335a`; fix `95247116` over `b9e19d07`). New rank-0 leaf `Source/Core/include/Interfaces/IAppOfflineQueue.h` — 9 pure virtuals (`TickOffline{Creates,FieldEdits}`, `Get{Pending,DeadPending}{Creates,FieldEdits}`, `GetPendingCreateCount`, `DeleteDeadPending{Creates,FieldEdits}`). Collection element types come from the rank-0 `CachedTicketTypes.h` (included); the two `Sync/OfflineQueueTypes.h` delete-summary return types (rank 2) are **forward-declared** — a rank-0 header can't include a `Sync/` header without a low→high back-edge, and a by-value return of an incomplete type is valid in a pure-virtual declaration. `AppController` implements the facet (base + `override` on the 9 existing decls). `BuiltinCommands_Offline.cpp` migrated off `AppController.h` → the facet + `CachedTicketTypes.h` + `Sync/OfflineQueueTypes.h`; `RegisterOfflineCommands` now takes `IAppOfflineQueue&`. **The rank-0-facet pattern (incl. the fwd-decl-the-higher-rank-return-type technique) is validated** — the include-cycle/layer-rank gate passes.

- **Batch 2 — `IAppMeta` + `IAppAttachments` + `IAppScenarios`** (PR [#1665](https://github.com/alexandrosk0/Smatchet/pull/1665), squash `9a40ff10`; fix `1ca7d0aa` over `671f7c20`). Three new rank-0 leaves under `Interfaces/`, migrating **four** command TUs off `AppController.h` — a clean **−4** now that the pilot established the dispatcher orchestrator include (§ Deviations).
  - `IAppMeta` (`GetAppVersion`, `GetGitHubReleaseRepo`, `RequestAppQuit`, `CheckForAppUpdate`) ← `BuiltinCommands_App.cpp` (now includes `Types/AppUpdateTypes.h` for the by-value `AppUpdateInfo` return; keeps `ConfigManager.h`).
  - `IAppAttachments` (`OpenAttachment`, `DownloadAttachmentForPreview`) ← `BuiltinCommands_Attach.cpp`.
  - `IAppScenarios` (`Scenarios()` + const overload) ← **both** `BuiltinCommands_Scenario.cpp` **and** `BuiltinCommands_UiTest.cpp`. **One-facet-two-TUs finding:** a facet need not be 1:1 with a command TU — both scenario TUs consume only `Scenarios()`, so a single facet migrates two includers. `ScenarioRunner`'s full definition still comes from `Commands/Scenarios/IScenario.h` (the facet just forward-declares `smatchet::cmd::ScenarioRunner` for the by-ref return).
  - **`IMainThreadPoster` two-param-registrar finding:** the two scenario TUs marshal to the UI thread via `RunOnUiThreadAsCommandResult`, which the pre-migration code fed with `AppController&` (implicitly an `IMainThreadPoster`). After narrowing the domain param to `IAppScenarios&`, they no longer carry a poster — so `RegisterScenarioCommands` / `RegisterUiTestCommands` take a **second** narrow ref `IMainThreadPoster& poster`, and the dispatcher passes `AppController&` twice (`reg, app, app`). A UI-marshaling command TU needs its domain facet **plus** `IMainThreadPoster`.
  - **Default-arg cleanup (CR nitpick, applied in fix `1ca7d0aa`):** `CheckForAppUpdate(bool includePrerelease = false)` and `DownloadAttachmentForPreview(..., std::string* outError = nullptr)` keep their defaults **only on the facet base**; the `AppController` `override` decls drop the redundant defaults. Default args are statically bound (not polymorphic), so a base/override drift would silently change behavior by static call type — keeping one copy removes the drift surface. All call sites pass explicit args, so behavior is identical.
  - **Cumulative fan-in reduction: pilot 0 (net-zero, foundational) + batch 2 −4 = −4.**

- **Batch 3 — `IAppUsers` + `IAppDebug`** (PR [#1668](https://github.com/alexandrosk0/Smatchet/pull/1668), squash `4bfb261d`). Two new rank-0 leaves under `Interfaces/`, migrating **two** command TUs off `AppController.h` — a clean **−2**.
  - `IAppUsers` (`SearchUsersByQuery`, `FetchIssueWatchers`, `FetchIssueVotes`) ← `BuiltinCommands_Users.cpp`. **rank-3 collection-element fwd-decl finding:** the element `TrackerUser` lives in the rank-3 `Tracker/TrackerFieldSchema.h`, which a rank-0 facet can't include without a low→high back-edge. It is **forward-declared** and the pure virtuals take `std::vector<TrackerUser>&` — a *reference* to a specialization does not require the element to be complete at the declaration point, so no instantiation is forced (the `Users.cpp` TU includes the full type). This **extends the pilot's fwd-decl technique from single by-value returns to `std::vector<incomplete>&` reference params** — validated by the green POSIX/MSVC compile lanes. Redundant `FetchIssueVotes` defaults (`outVoteCount`/`outHasVoted`/`outVotersInResponse = nullptr`) dropped from the `AppController` override (kept on the facet base); the one non-facet caller (`TrackerGridFieldDisplay`) already passes all six args explicitly.
  - `IAppDebug` (`AddAutomationLogSink`, `AddAutomationErrorSink`, `ConsumeScriptingWindowRequest`, `ExecuteLuaConsoleSnippet`; **plus** `CopyMcpActivityLog`, `TryGetMcpLastClientHttpActivity` under `#if SMATCHET_WITH_MCP`) ← `BuiltinCommands_Debug.cpp`. **Conditional-virtual-facet finding:** the two MCP methods are guarded by `#if SMATCHET_WITH_MCP` **identically** on the facet's pure virtuals and the `AppController` overrides, so the abstract surface matches the concrete class in every build config — validated by the **green `Windows + MSVC (Smatchet light — MCP off)` lane**, which compiles the MCP-methods-absent facet shape. Reuses the batch-2 `IMainThreadPoster` two-param seam (the four dock/window commands marshal to the UI thread → `RegisterDebugCommands(reg, IAppDebug&, IMainThreadPoster&)`, dispatcher passes `app, app`). **Pillar-1 note:** `ConsumeScriptingWindowRequest` is called once per frame from the LuaConsole plugin, but through a concrete `AppController&` and it is an out-of-line atomic exchange (never inlined), so virtualizing it costs at most one vtable lookup per frame — not the inlined-hot-getter case the exclusion protects.
  - **`Perf` excluded (not facet-migratable):** `BuiltinCommands_Perf.cpp` reads the **public** `mainThreadDispatcher` data member directly (`app.mainThreadDispatcher.QueueLen()`, …) and passes `AppController&` to the free function `ProcessGridFieldEdits(AppController&, …)` — both need the complete type, so it stays on `AppController.h`. Documented so a future slice doesn't re-attempt it without first adding accessors / narrowing `ProcessGridFieldEdits`.
  - **Cumulative fan-in reduction: pilot 0 + batch 2 −4 + batch 3 −2 = −6.**

## Deviations from plan

- **The dispatcher is the irreducible orchestrator-includer → facet #1 is NET-ZERO on the fan-in count, not −1.** The plan's § Files-to-modify pilot claimed "Fan-in count −1". Implementation surfaced that the thin dispatcher `Source/Core/src/Commands/BuiltinCommands.cpp` only *forward-declares* `AppController` (fine for the 21 registrars taking `AppController&`), but the `AppController& → IAppOfflineQueue&` derived-to-base conversion for a facet-migrated registrar needs the **complete** type. So the dispatcher must `#include "AppController.h"` (with a `SMATCHET_DEVIATION(rule=app-controller-fan-in)` — CI caught this as an incomplete-type error on the first push). Net for the pilot: `BuiltinCommands_Offline.cpp` −1, dispatcher +1 = **0**. This is a one-time cost that **unlocks a clean −1 for every subsequent facet** (the dispatcher already includes it; each further leaf command TU just drops off). The campaign still targets ~14 command-TU includers → 1 orchestrator. **Plan implication:** the fan-in *reduction* begins at facet #2; facet #1 is foundational (validates the pattern + establishes the orchestrator include).
- **`BASELINE_FAN_IN` constant left unchanged** (documented 115 vs live ~119 — already stale, drifted up by prior features). The merge-base `--diff` set-difference ratchet is the real enforcement and it passes; resyncing the informational constant is a separate cleanup.

## Verification (actual)

- **Build gate (the real verification — no Core TU compiles in this Linux container):** CI on the pilot fix SHA `95247116` was **fully green** — `Windows + MSVC` (full), `Windows + MSVC (Smatchet light)`, `Windows + MSVC (ARM64 cross-compile)`, `Mobile — POSIX core compile gate (Linux clang)` (compiles `BuiltinCommands_Offline.cpp` against the facet under Lua-OFF), `Mobile — Android NDK / APK / emulator smoke`, `Sanitizer (ASAN)` + `(UBSan)`, `Bucket-C` + `Bucket-E ×2`, `Perf PR-fast`, `Coverage`, `CodeQL`, `Duplication scanner`, `C++ lint`. CodeRabbit: "Review completed" + "CR findings (0 actionable)". Cursor Bugbot: neutral (usage-cap, no wedge).
- **The 9 `override` signatures were cross-checked** against the facet's pure virtuals before push (`size_t` ≡ `std::size_t`; const / return-type / params all match).
- **Local:** repo lint gate green — include-cycle/layer-rank (rank-0 facet, no back-edge), fan-in ratchet (dispatcher's new include deviation-suppressed; `BuiltinCommands_Offline.cpp` removed), dup, comment-noise.
- **First-push CI catch:** the dispatcher incomplete-type error (see § Deviations) — fixed in one round-trip; the fast Android lane surfaced it in ~1.5 min.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip § Status to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.

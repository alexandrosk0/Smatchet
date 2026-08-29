# Plan — N4 TrackerActions interface (assessment + verdict)
<!-- plan-date: 2026-07-13 -->

> **Slug**: `n4-trackeractions-interface` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — verdict delivered and applied (N4 closed in the ledger's 2026-08-18
> reconciliation; the ledger itself was retired 2026-08-29, and this assessment archived per its
> own outstanding-bookkeeping note). Originally an **assessment plan**: it evaluates whether the `TrackerActions` interface that BACKLOG_CODE_REVIEW.md N4 calls "the overdue Phase 2 step" is still worth building, given six shipped fan-in phases + the deps-adapter extraction that post-date the N4 ledger note. Verdict below is **do not build TrackerActions; close N4** — but the plan documents the exact residual coupling surface so a future author can re-open with eyes open if the friend ever regrows.

## Context

BACKLOG_CODE_REVIEW.md N4 (verified 2026-07-13: `AppController.h` is 1514 LOC) asks for two things: **(A)** move cross-concern DTOs out of `AppController.h` into their owning service headers, and **(B)** introduce a `TrackerActions` interface to replace the `friend`/private-reach-through coupling the ledger flagged as a "code-smell siren."

Both halves have been overtaken by work that shipped AFTER the ledger's 2026-07-05 note:

- **Part A is already done.** Every DTO the ledger named is out of `AppController.h`:
  - `TrackerIssueFetchPack` + `TrackerConnectivityBannerForUi` → `Sync/SyncTypes.h` (fan-in Phase 3).
  - `AppUpdateAsset` / `AppUpdateInfo` → `Types/AppUpdateTypes.h` (fan-in Phase 3).
  - The six offline-queue `*Summary` structs → `Sync/OfflineQueueTypes.h` (re-exported via in-class `using`-aliases so `AppController::DeadLetterRestoreSummary` etc. keep resolving).
  - `FieldEditResult` → `Types/FieldEditTypes.h`; `TrackerConnectivityState` + `AttachmentDescriptor` → `Types/`.
  - The ONLY inline struct definitions left in `AppController.h` are `AppController::BackgroundWorker` (line 1428) and `AppController::AutomationJob` (line 1460) — both **private nested POD types owned by AppController itself** (used only in `AppController*.cpp` + `AppControllerImpl.h`), not cross-concern service DTOs. Moving them out would break their `AppController::`-qualified names for zero fan-in benefit — a net-negative churn, explicitly NOT recommended.
  - Fan-in is **71** includers today (`agents/scripts/core/appcontroller_fan_in_audit.py`), down from the documented baseline of 115.

- **Part B's premise has shifted.** The ledger describes "three friends collapsed into one `GridContextDepsAdapter`." The extraction went much further: `GridContextDepsAdapter` now implements **six** deps interfaces (`IOfflineQueueDeps`, `ITicketSyncDeps`, `IEditMetaDeps`, `IFieldEditDeps`, `IConnectivityDeps`, `IAttachmentAppUpdateDeps`), and `AppController` itself now implements **13 narrow `IApp*` facet interfaces** (`docs/plans/appcontroller-fan-in-phase5-facets.md`, `docs/plans/appcontroller-fan-in-phase6-dissolution.md`). The single remaining `friend class GridContextDepsAdapter;` is the ONE reach-through left, and it is already interface-mediated on the consumer side — the services hold `IOfflineQueueDeps& / ITicketSyncDeps&` references and tests substitute `FakeOfflineQueueDeps` / `FakeTicketSyncDeps`.

Intended outcome — after this assessment lands, N4 is closed with a documented verdict: **`TrackerActions` would add an interface layer without removing the one friend it targets; the deps-adapter pattern already delivers the testability + decoupling `TrackerActions` was meant to deliver.**

## Approach

**Recommended: do NOT build `TrackerActions`. Close N4 as substantially-resolved.**

The `TrackerActions` interface (old §1.7 design proposal #4) predates the deps-adapter architecture. Its goal was to give the extracted services (offline-queue, ticket-sync) a *typed contract* against AppController instead of `friend` access to every private member. That goal is **already met by the six `I*Deps` interfaces** — they ARE the typed action contract, and they are the seam tests fake. Adding `TrackerActions` on top would be a second, overlapping abstraction over the same state.

The residual `friend class GridContextDepsAdapter;` is structurally different from what N4 imagined. It is not a service reaching into AppController — it is the **adapter that IMPLEMENTS the deps interfaces**, and by design it forwards a closed, enumerated set of private members/helpers (see § Residual coupling surface). One friend on the adapter is the correct shape: the adapter is the single translation layer between `GridLiveContext` per-context state + AppController global state and the interface contracts the services consume. Replacing that friend with a `TrackerActions` interface would mean either (a) promoting ~10 private helpers to public/interface methods — widening the very surface N4 wants to shrink — or (b) leaving the friend in place anyway for the private-data-member reads, achieving nothing.

The non-obvious trade-off: the friend looks alarming ("every private member is public to the adapter") but the adapter is a ~166-line, single-purpose, AppController-owned class whose entire job IS to touch those members. A friend scoped to one owned translation class is a normal C++ idiom, not the "public to 70% of the codebase" smell the original N4 note described (that smell was the THREE service friends, now gone).

## Residual coupling surface (the exact friend reach-through — the thing TrackerActions would target)

`GridContextDepsAdapter` (`Source/Core/src/GridContextDepsAdapter.cpp`) reaches through `friend` to this **closed set** of AppController privates. Classified by whether a `TrackerActions` interface could plausibly remove the friend for that access:

**Private data members read directly (friend genuinely required; an interface would need a getter each — widening surface, not shrinking it):**
- `app_.backendFactory_` — `ITrackerBackendFactory*` for backend re-creation on tracker swap.
- `app_.connectivity_` — the `ConnectivityMonitorService` unique_ptr (drives the FSM forwards).
- `app_.hostCallbacks_` — `const HostCallbacks&` for OpenUrl / attachment host hooks.
- `app_.offlineQueue_` — the `OfflineQueueService` unique_ptr (replay-timer pushes).
- `app_.pendingLuaWindowBump_` — `bool` flag get/set for Lua-window invalidation coalescing.
- `app_.requestDeferredLiveTrackerBackendSuccessNotify_` — the const notify helper.
- `app_.fieldCatalog()` — private `GridContextFieldCatalog&` accessor (line 1286).

**Private helper methods called (a facet COULD expose these, but they are AppController-internal mechanics, not a tracker-action vocabulary):**
- `app_.RefreshLocalDataCheckedImpl_(ctx, gen)` — the generation-checked refresh (issue #1081); deliberately private so every checked caller passes the latched `GridLiveContext`.
- `app_.RetireBackend(old)` — parks a swapped-out backend as a defer-free husk.
- `app_.PruneEditMetaCacheToActiveTickets()`, `app_.WarmIssueTypeEditMetaAtStartAsync(cfg)` — edit-meta cache lifecycle.

**Already-public methods (NOT a friend reach — listed for completeness; a `TrackerActions` interface would be redundant with these):** `BackendShared`, `Cache`, `FindFieldById`, `GetActiveTicketsSnapshot`, `GetRequiredFieldSet`, `IsShuttingDown`, `LaunchBackgroundTask`, `NotifyLuaTicketDataChanged`, `OpenUrl`, `RefreshLocalData`, `RequestAppQuit`, `UpdateTicket`, `PushOfflineReplayTimersDuringTransportOutage`.

**`GridLiveContext` per-context members** (`ctx_.Backend`, `ctx_.ActiveTickets`, `ctx_.fieldCatalog`, `ctx_.backendGeneration_`, `ctx_.initialSyncKicked`, `ctx_.lastSyncedJql`, `ctx_.syncRetryAfter`, …) — these are on `GridLiveContext`, not AppController; a `TrackerActions` interface on AppController would not touch them at all.

**Takeaway:** of the ~23 `app_.` accesses, 13 are already public methods (no friend needed), 7 are private-data-member reads (a friend or a widening getter — no net win), and 3 are private mechanics helpers. A `TrackerActions` interface would remove the friend ONLY if all 10 private accesses were promoted to interface methods — which widens `AppController`'s public/virtual surface by 10 members to delete one `friend` line on an owned 166-line class. That trades a contained idiom for a wider ABI. Not worth it.

## Proposed interface (documented for completeness — NOT recommended to build)

Were `TrackerActions` built anyway, its minimal honest shape (the private-only accesses; the 13 public methods need no interface) would be:

```cpp
// Interfaces/ITrackerActions.h  (rank-0 leaf; forward-declares heavy types)
class ITrackerActions {
  public:
    virtual ~ITrackerActions() = default;
    virtual ITrackerBackendFactory* BackendFactory() = 0;
    virtual ConnectivityMonitorService& Connectivity() = 0;
    virtual OfflineQueueService& OfflineQueue() = 0;
    virtual const HostCallbacks& Host() const = 0;
    virtual GridContextFieldCatalog& FieldCatalog() = 0;
    virtual void RefreshLocalDataChecked(GridLiveContext&, std::uint64_t gen) = 0;
    virtual void RetireBackend(std::shared_ptr<ITrackerBackend>) = 0;
    virtual void PruneEditMetaCacheToActiveTickets() = 0;
    virtual void WarmIssueTypeEditMetaAtStartAsync(TrackerConfig) = 0;
    virtual bool GetPendingLuaWindowBump() const = 0;
    virtual void SetPendingLuaWindowBump(bool) = 0;
    // + requestDeferredLiveTrackerBackendSuccessNotify() const
};
```

This is not a "tracker-actions vocabulary" — it is a grab-bag of AppController's internal plumbing, which is exactly why exposing it as an interface is the wrong abstraction. The clean interfaces already exist (the six `I*Deps`); this would be a leaky twelfth.

## Slice breakdown (if a future author over-rules the verdict)

Each slice is behavior-preserving + independently shippable + testless-extraction-proof (characterization test FIRST, per the ledger's "testless extraction = lottery" warning):

1. **Slice 0 — characterization harness (MUST land first).** Bucket-A doctest driving `GridContextDepsAdapter` against a fake AppController + fake `GridLiveContext`, asserting every forwarded method's current behavior (backend swap re-creates via factory; `RefreshLocalData(gen)` routes the latched ctx; replay-timer push idempotence; the shared-override dispatch for the 3 methods declared in ≥2 interfaces). Without this, slices 2-4 are unverifiable — the whole point of the ledger warning. **Gate: this slice ships alone and green before any interface work.**
2. **Slice 1 — `ITrackerActions.h` rank-0 leaf header** (the interface above), `AppController` adds it to its base list, out-of-line trivial overrides in `AppController.cpp`. Zero behavior change; fan-in ratchet must stay flat.
3. **Slice 2 — retype the private-data reaches.** `GridContextDepsAdapter` stores `ITrackerActions& actions_` alongside `app_`; swap the 7 private-member reaches + 3 helper calls to `actions_.*`. Keep `app_` for the 13 public-method calls (no reason to route those through the interface).
4. **Slice 3 — drop `friend class GridContextDepsAdapter;`.** Only possible if slice 2 removed EVERY private access. Re-run the friend-reach audit (`grep -oE 'app_\.\w+'` vs the public-method list) to confirm zero private reaches remain; if any survive, the friend stays and the whole exercise delivered nothing — the tripwire that should abort the effort.

The slice-3 abort condition is the honest reason the verdict is "don't build": slice 2 cannot promote the private-data reads without adding a public getter for each, and those getters ARE the surface widening. The friend does not actually go away cheaply.

## Golden / characterization tests each slice needs FIRST

- **Slice 0**: `tests/Core/GridContextDepsAdapter.test.cpp` — fake-backed doctest (per the existing `FakeOfflineQueueDeps` / `FakeTicketSyncDeps` precedent in the ticket-sync + offline-queue unit tests). This is the SAME fake-deps infrastructure the extraction already built, so the harness is cheap.
- **Slices 1-3**: no NEW test — slice 0's harness IS the regression gate (behavior-preserving refactor: the characterization suite must stay byte-identical-green across all three).
- **Backstop**: the nightly Lua-OFF + sanitizer builds catch config-skew (a member accessed only under `SMATCHET_WITH_LUA_AUTOMATION` — `pendingLuaWindowBump_` is Lua-adjacent).

## Risk / fan-in assessment

- **Fan-in**: NEUTRAL. Adding `ITrackerActions.h` to AppController's base list does not change `AppController.h`'s includer count (the interface header is included BY AppController.h, not by the 71 includers). The adapter already includes its six deps headers; a seventh is free. So the fan-in ratchet (`app-controller-fan-in`, ratchet-down-only, hard-fail) stays flat — neither helps nor hurts N4's stated size concern.
- **AppController.h LOC**: NEUTRAL-to-WORSE. The interface adds ~1 base-class line + the override decls are already-declared public methods where they overlap; net the header does NOT shrink. N4's "1512 LOC and growing" concern is unaddressed by `TrackerActions` — the LOC is method DECLARATIONS + doc comments, not DTO definitions, and those declarations don't move.
- **ABI/surface risk**: WORSE. Promoting 10 private members to virtual interface methods widens the class's virtual surface + adds vtable entries (Pillar-1: none of these are per-frame hot getters, so no perf risk, but the surface-widening is real).
- **Behavior risk**: the shared-override subtlety (3 methods declared in ≥2 deps interfaces satisfied by one override) is fragile; a `TrackerActions` refactor that reshuffles overrides could silently break multi-pane focus behavior (Phase 3 R1 hazard, called out in `GridContextDepsAdapter.h` lines 139-146). This is why slice 0's characterization suite is non-negotiable.

**Net verdict**: `TrackerActions` costs a wider virtual ABI + a new interface, delivers no fan-in reduction, does not shrink `AppController.h`, and can only remove the one friend by widening the surface elsewhere. The deps-adapter architecture already provides the testable decoupling. **Recommend: mark N4 substantially-resolved (Part A done; Part B moot), and re-open ONLY if a NEW service is extracted that would otherwise need a second friend — at which point the deps-interface pattern (not `TrackerActions`) is the proven answer.**

## Files to modify

**This assessment plan modifies no product code.** It is a verdict doc. The (not-recommended) implementation would touch:
1. `Source/Core/include/Interfaces/ITrackerActions.h` — NEW rank-0 interface (grep-confirmed absent: `rg -l 'ITrackerActions' Source/` → no hits today).
2. `Source/Core/include/AppController.h:173` — the `friend` line (slice 3 removal target) + base-class list.
3. `Source/Core/include/GridContextDepsAdapter.h` + `Source/Core/src/GridContextDepsAdapter.cpp` — store + use `ITrackerActions&`.
4. `tests/Core/GridContextDepsAdapter.test.cpp` — NEW characterization harness (slice 0).

## Existing utilities reused

- `FakeOfflineQueueDeps` / `FakeTicketSyncDeps` — the extraction's existing fake-deps test doubles (ticket-sync + offline-queue unit tests) — slice 0's harness reuses this pattern rather than inventing new fakes.
- `agents/scripts/core/appcontroller_fan_in_audit.py` — the fan-in ratchet audit (verify NEUTRAL delta).
- The six `I*Deps` interfaces (`IOfflineQueueDeps.h`, `ITicketSyncDeps.h`, `IEditMetaDeps.h`, `IFieldEditDeps.h`, `IConnectivityDeps.h`, `IAttachmentAppUpdateDeps.h`) — the proof that the typed-contract goal is already met.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — this is an assessment/verdict plan; it extracts nothing. (The Part-A DTO extractions it references already shipped in fan-in Phases 1-6; this plan only documents that they are done.)

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — the assessment recommends no code change; even the not-recommended interface touches only cold construction/wiring paths, no per-frame getters virtualized.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: no impact — no I/O or blocking path added.
- **Pillar 3 (never crash)**: no impact — no ownership/lifetime change; the plan explicitly preserves the adapter's dtor-ordering contract (`GridContextDepsAdapter.h` lines 10-14).
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A — this plan's PR is docs-only (`docs/plans/n4-trackeractions-interface.md`). No `Source/Core/` diff. (Were the not-recommended implementation built, the touched paths are cold wiring — gate 1 maps to no hot scenario; gates 2-5 do not fire.)

## Risks / non-goals

- **Risk — the friend regrows.** If a future extraction adds a service that reaches into AppController privates, someone may add a second friend, resurrecting the original N4 smell. Mitigation: the deps-interface pattern is the documented answer (this plan) — a new service gets a new `I*Deps` + an adapter method, never a second friend.
- **Non-goal — shrinking `AppController.h` LOC.** N4 conflates "friend coupling" with "1512 LOC." The LOC is driven by public method declarations + doc comments, which the fan-in phases deliberately kept (they moved DEFINITIONS/DTOs out, not the delegator declarations). Shrinking the header further is the fan-in program's terminal-ceiling question (`docs/plans/appcontroller-fan-in-phase6-dissolution.md` § T6), not a `TrackerActions` question.
- **Non-goal — moving `BackgroundWorker` / `AutomationJob` out.** They are private nested types owned by AppController; relocating them breaks `AppController::`-qualified names for zero benefit.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. This plan's own PR is docs-only:

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A for this docs-only plan. (The not-recommended slice 0 would add `tests/Core/GridContextDepsAdapter.test.cpp`.)
- **Bucket E (ImGui Test Engine)**: N/A — no UI change.
- **Bash-driver scenario / screenshot / sanitizer**: N/A — no runtime change.
- **Build gate**: N/A for the docs-only PR (no C++ touched). The Part-A verification (that the DTOs already moved cleanly) is proven by develop already building green post-Phase-6.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this verdict against the domain model + the fan-in Phase 5/6 plans before finalising; record the outcome. (Key grill question: does any of the 71 remaining includers reach a tracker-action AppController would want to abstract behind `TrackerActions` rather than a facet? Answer from the audit: no — the tracker-action calls all go through `ITracker*` role interfaces or the `IApp*` facets already.)
- **Manual residue**: none — verdict plan, no manual step.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray "TrackerActions" / "Phase 2 interface" references that assume this work is pending, and revise them to point at this verdict.

- **The fan-in terminal-ceiling question** (can `AppController.h`'s 71 includers drop further?) — owned by `docs/plans/appcontroller-fan-in-phase6-dissolution.md` § T6 (verdict there: terminal by design, blocked on Pillar-1 inline getters). Not re-opened here.
- **`GridLiveContext` de-singletoning** (multi-grid Slice 3, ADR-0018) — the per-context state the adapter's `ctx_` half forwards to; a separate program.

## Implementation log
- `0cbe593e` · archived to `shipped/` during the 2026-08-29 review-ledger retirement; the
  verdict itself ("do not build TrackerActions; close N4") had been applied by the ledger's
  2026-08-18 reconciliation, per `git show 0cbe593e~1:backlog/BACKLOG_CODE_REVIEW.md`
  § "Net open: zero work items" (the ledger was retired in that same `0cbe593e`).

## Deviations from plan
- None. The plan's deliverable was the assessment verdict, and the verdict was "don't
  build" — no code shipped, so there was nothing to deviate from. The residual coupling
  surface it documents remains available for a future re-open.

## Verification (actual)
- No production diff to verify (assessment-only plan). Archival verified against the
  documentation gates at the archiving commit: `test-plan-index.sh` (INDEX regenerated,
  in sync), `test-plan-ref-integrity.sh` (all refs resolve; the plan's self-reference
  moved to the tier-less form), `test-markdown-links.sh --all` (0 dangling), `md_lint.py`
  clean. Deferral residue-sweep run at archival: no stray "TrackerActions" /
  "Phase 2 interface" references outside this plan in `**/CONTEXT*.md`, `docs/adr/`,
  `agents/`, or `docs/self-improvement/categories/`.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip the § Status header to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.

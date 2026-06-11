# Plan — Per-pane backend picker (choose which tracker a new grid pane loads)

> **Slug**: `pane-backend-picker` (matches this file's basename without `.md`).
>
> **Status**: `active` — design draft, not yet implementation-approved. Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s active → shipped (see § Archive).
>
> **Usage**: every section filled; non-applicable sections carry `N/A — <reason>`, never deleted.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

[`multi-grid-tabs`](multi-grid-tabs.md) shipped the **engine** for side-by-side different-backend panes: each pane carries its own `(backendKey, viewId)`, owns a live `GridLiveContext` (its own `ITrackerBackend` + sync + ticket cache), and `tickets_v2` / offline-queue rows are `backend_key`-namespaced. The plan's stated motivation (line 13) was a user who "track[s] work across two projects, two query-views, or two trackers (e.g. a Jira board + a GitHub repo) [and] cannot see them at once."

**The gap**: there is **no UI affordance to put a *different* backend into a new pane.** Every pane-creation path is a *duplicate-the-focused-pane* op — the `+` button, `pane.new`, `pane.duplicate`, `pane.split` all copy the source pane's `backendKey` + `viewId` ([`SmatchetGridPaneWindows_detail.cpp:88-95`](../../../Source/Core/src/Ui/SmatchetGridPaneWindows_detail.cpp:88); [`PaneCommands.cpp:155`](../../../Source/Core/src/Commands/PaneCommands.cpp:155)). The only way to *reach* a cross-backend pane today is implicit and non-obvious: focus a pane, open **Preferences**, switch the global tracker; the focused pane's steady-state then rewrites `pane.backendKey = NormalizeViewsBackendKey(cfg.TrackerType)` ([`SmatchetGridPaneWindows.cpp:312-314`](../../../Source/Core/src/Ui/SmatchetGridPaneWindows.cpp:312)). So the multi-tracker side-by-side *capability* was built but the *selection UX* was never designed — confirmed: no picker/chooser/backend-selector appears in `multi-grid-tabs.md`, its slice1-design addendum, or [ADR-0018](../adr/0018-multi-grid-pane-contexts.md).

This plan closes that gap: a discoverable affordance to create a new pane **bound to a chosen backend** (and, optionally, a chosen saved view), plus the matching `pane.new` command parameters so CLI / MCP / Lua / Scenarios can do the same.

**What already exists and is reused, not rebuilt**:
- The cross-backend focus → `cfg.TrackerType` re-point + per-context swap chokepoint ([`SmatchetGridPaneWindows.cpp:237-296`](../../../Source/Core/src/Ui/SmatchetGridPaneWindows.cpp:237)) — a newly-created cross-backend pane uses it on first focus, unchanged.
- `EnsurePaneContextLive(paneId, backendKey)` + `EnsurePaneLiveSyncStarted` — spin up the new pane's context + sync (Slice 3 of `multi-grid-tabs`).
- The `paneAddRequestSourceId` latch + `ApplyPaneAddAndCloseRequestsCore` — the deferred-apply seam that already creates panes safely (never mid-frame).
- Per-backend view buckets (`ConfigManager` v2 `backends` map; `EnsureViewBucketBootstrapped` / `LoadViewsOrBootstrap`) — supply the chosen backend's default/first view.

**Scope boundary**: this is purely the *creation-time selection* affordance + its command surface. It does **not** add multi-profile (two Jira servers), cross-pane aggregation, or change the focused-pane / cfg-follows-focus model — all out of scope below.

## Approach

**Three known backend keys today**: `Jira` (= `SmatchetDefaults::kDefaultBackendType`, the catch-all), `Plane`, `GitHub` — the closed set `NormalizeViewsBackendKey` collapses to ([`ConfigManager_Views.cpp:225-237`](../../../Source/Core/src/Config/ConfigManager_Views.cpp:225)). The picker enumerates this set (forward-compat: Linear slots in as a fourth once [`linear-tracker-backend.md`](linear-tracker-backend.md) lands — the enum is the single source).

**Core mechanism — extend the add-request latch to carry an optional target.** Today `paneAddRequestSourceId` is a bare `std::string` (source pane id); `ApplyPaneAddAndCloseRequestsCore` copies `src->backendKey/viewId`. Replace it with a small request value:

```cpp
struct PaneAddRequest {
    std::string sourceId;       // pane to duplicate (focused pane); always set
    std::string targetBackendKey;  // empty = duplicate source's backend (today's behaviour)
    std::string targetViewId;      // empty = backend's default/active view
};
```

Empty `targetBackendKey` preserves the current duplicate-source semantics byte-for-byte (the existing `addRequestSourceId.empty()` "no request" sentinel becomes `sourceId.empty()`). A non-empty `targetBackendKey` means "create a pane on **this** backend": the apply core sets `dup.backendKey = targetBackendKey` and resolves `dup.viewId` = `targetViewId` if given, else the backend's active/first view from its view bucket (bootstrapping the bucket if the backend was never opened). The new pane does **not** inherit the source's snapshot pointer (different backend → different data); its `GridLiveContext` spins up on first visible frame via the existing `EnsurePaneContextLive` path.

**UI surface — split `+` into "duplicate" + "new on backend…".** The pane host's `+` keeps its current one-click *duplicate focused pane* behaviour (no regression, no extra click for the common case). A small adjacent caret (`+ ▾`) opens a popup listing the configured backends — `New Jira pane`, `New Plane pane`, `New GitHub pane` — each with a submenu / second level of that backend's saved views (default highlighted). Selecting one writes a `PaneAddRequest{ sourceId=focused, targetBackendKey=<chosen>, targetViewId=<chosen-or-empty> }`. Rationale for split-button over a modal: zero added friction for duplicate (the dominant action), one extra click to pick a backend, fully keyboard-navigable (Pillar 4), and it reuses ImGui's native popup/menu — no bespoke modal lifecycle. (Modal alternative weighed + rejected in § Risks.)

**Command surface — additive `pane.new` params.** `pane.new` (and only `pane.new`; `duplicate`/`split` stay pure-duplicate by name) gains two optional params:
- `backend` (enum: the backend-key set) — empty/absent = duplicate focused (unchanged).
- `view` (string, view id) — empty/absent = chosen backend's default view.

Both flow into the same `PaneAddRequest`. Because every front-end dispatches through `CommandRegistry`, CLI / Command Palette / MCP schema / Lua / Scenarios pick the params up from one registration (Commands-subsystem invariant). `RegisterPaneAddCommand`'s `acceptDirection` pattern extends naturally to accept these.

**Credential gating (design decision — see § Risks)**: the picker lists the full known-backend set; a backend with no credentials configured is shown **disabled with a "configure in Preferences" hint** rather than hidden (discoverability > tidiness — a hidden GitHub entry reads as "not supported"). Creating a pane on a credential-less backend is blocked at selection, not deferred to a failed sync.

### Slices

Small feature; three thin slices, each independently shippable.

**Slice 1 — request-latch + apply-core (bucket-A, no UI):** introduce `PaneAddRequest`; migrate `paneAddRequestSourceId` → `paneAddRequest`; teach `ApplyPaneAddAndCloseRequestsCore` the target-backend/view resolution (bucket bootstrap, default-view pick, no snapshot inherit on cross-backend). Pure-logic; fully bucket-A testable. Behaviour-identical when target fields empty.

**Slice 2 — `+ ▾` split-button UI + view submenu:** the host popup; backend list (enabled/disabled by credential presence); per-backend view submenu; writes the request. Bucket-E smoke + visual-validation pause (touches `Smatchet*Ui*.cpp`).

**Slice 3 — `pane.new` `backend`/`view` params:** wire the command params into the request; MCP schema + Lua binding pick up from the registry; a Scenario step exercises create-on-backend. Bucket-A over the handler arg→request mapping.

## Files to modify

**Slice 1 — request latch + apply core:**
1. [`Source/Core/include/Ui/SmatchetUiSession.h`](../../../Source/Core/include/Ui/SmatchetUiSession.h) — replace `std::string paneAddRequestSourceId` with `PaneAddRequest paneAddRequest` (new struct, same header or a small `GridPane.h` adjacency). Keep an `.empty()`-equivalent (`sourceId.empty()`) sentinel.
2. [`Source/Core/src/Ui/SmatchetGridPaneWindows_detail.cpp:56-103`](../../../Source/Core/src/Ui/SmatchetGridPaneWindows_detail.cpp:56) — `ApplyPaneAddAndCloseRequestsCore` reads the new struct; target-backend branch (set `dup.backendKey`, resolve `dup.viewId`, skip snapshot inherit when `targetBackendKey != src->backendKey`). Pure — stays bucket-A friendly.
3. New free helper (same detail TU) — `ResolveNewPaneView(backendKey, requestedViewId, viewBuckets) -> viewId`: requested-id-if-valid → backend active view → backend first view → bootstrap. Pure; bucket-A.
4. [`Source/Core/src/Ui/SmatchetGridPaneWindows.cpp`](../../../Source/Core/src/Ui/SmatchetGridPaneWindows.cpp) + [`PaneCommands.cpp:155`](../../../Source/Core/src/Commands/PaneCommands.cpp:155) — update the two existing write sites (`+` button, `pane.*` commands) to populate `paneAddRequest.sourceId` (mechanical rename; behaviour unchanged in this slice).

**Slice 2 — UI:**
5. [`Source/Core/src/Ui/SmatchetGridPaneWindows.cpp`](../../../Source/Core/src/Ui/SmatchetGridPaneWindows.cpp) — the `+ ▾` split-button + backend popup + per-backend view submenu in the pane-host title-bar/tab-bar area; credential-presence enable/disable. (Watch the 200-line ImGui-draw cap — extract a `DrawNewPaneMenu` helper per the draw-pattern guide.)
6. Backend-enumeration helper — a small `KnownBackendKeys()` (or reuse an existing list if one exists; grep first) so the picker + the `pane.new` enum share one source. Likely beside `NormalizeViewsBackendKey` in `ConfigManager_Views.cpp`.
7. Credential-presence query — a `bool ConfigManager::HasCredentialsFor(backendKey)` (or reuse existing per-backend credential checks; verify) for enable/disable.

**Slice 3 — command params:**
8. [`Source/Core/src/Commands/PaneCommands.cpp:136-167`](../../../Source/Core/src/Commands/PaneCommands.cpp:136) — `RegisterPaneAddCommand` for `pane.new` accepts `backend` (enum from the shared key list) + `view`; map into `paneAddRequest`. `duplicate`/`split` unchanged.
9. MCP tool schema + Lua binding — **no hand-edit expected** (registry-driven); verify the new params surface and add a Scenario step if a `pane.*` scenario exists.

## Existing utilities reused

- `ApplyPaneAddAndCloseRequestsCore` + the `paneAddRequest*` latch — [`SmatchetGridPaneWindows_detail.cpp:56`](../../../Source/Core/src/Ui/SmatchetGridPaneWindows_detail.cpp:56): the deferred-apply seam; extended, not replaced.
- `EnsurePaneContextLive` / `EnsurePaneLiveSyncStarted` — multi-grid Slice 3: the new pane's context + sync spin-up, unchanged.
- Cross-backend focus re-point chokepoint — [`SmatchetGridPaneWindows.cpp:237`](../../../Source/Core/src/Ui/SmatchetGridPaneWindows.cpp:237): a new cross-backend pane uses it on first focus; no new code.
- `NormalizeViewsBackendKey` + per-backend view buckets — [`ConfigManager_Views.cpp:225`](../../../Source/Core/src/Config/ConfigManager_Views.cpp:225) / [`:323`](../../../Source/Core/src/Config/ConfigManager_Views.cpp:323): backend-key set + default-view source.
- `RegisterPaneAddCommand` `acceptDirection` param-spec pattern — [`PaneCommands.cpp:144`](../../../Source/Core/src/Commands/PaneCommands.cpp:144): the template for the new `backend`/`view` params.
- `PaneViewSelfRepairAllowed` / `ChoosePaneColumnsSource` — [`SmatchetGridPaneWindows_detail.cpp:105`](../../../Source/Core/src/Ui/SmatchetGridPaneWindows_detail.cpp:105): a cross-backend new pane is already handled by these (its viewId is valid in its own bucket; render fallback until first focus).

## UX Pillar callouts

- **Pillar 1 (perf, 6.94 ms)**: pane creation is a one-shot user action, not a steady-state path. One added `GridLiveContext` spin-up on first visible frame is already covered by the `side-by-side-2-grid` perf scenario (multi-grid Slice 5b). The popup itself is a stock ImGui menu (negligible). No new hot path.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: the new pane's first sync runs on its own worker via the existing `EnsurePaneLiveSyncStarted` `/* PILLAR2_WORKER_ONLY */` path — zero new sync/HTTP/SQLite on the UI thread. Bucket bootstrap for a never-opened backend reads the views file; if that can land on the UI thread it must be flagged + deferred (the existing `LoadPersistentViewsFromDisk` already `WarnIfOnUiThread`s — reuse, don't add a new blocking read).
- **Pillar 3 (never crash)**: the request applies through the existing deferred latch (never resizes `gridPanes` mid-frame); a chosen view that fails to resolve falls back to the backend default then bootstrap (never a dangling `viewId`); a credential-less backend is blocked at selection. No new `std::thread`/backend ownership.
- **Pillar 4 (accessibility)**: the split-button + popup must be keyboard-reachable (the popup opens on the caret, arrow-navigable, Enter-selects); disabled entries carry the "configure in Preferences" tooltip. Bucket-E reachability backlogged per Pillar-4 status; the visual change triggers the visual-validation pause at ship.

## Memory impact

Negligible new resident cost. A pane created on a *different* backend is one more live `GridLiveContext` (snapshot + grid runtime) when visible — identical to today's cross-backend pane, already sized in `multi-grid-tabs.md` § Memory impact (≈ 0.5–2.5 MB / 500-ticket pane, hidden-LRU-evictable). The picker adds no persistent structure beyond the existing `smatchet_panes.json` rows. `N/A` for new telemetry — the multi-grid `GridPaneCount` / `VisiblePaneCount` gauges already cover it.

## Performance

Steady-state budget unchanged (6.94 ms / 144 Hz; p99 ≤ 10.0 ms). This plan adds **no steady-state path** — pane creation is a discrete user action and the popup is stock ImGui. The only runtime addition is one `GridLiveContext` spin-up + first sync on creating a cross-backend pane, which is the exact path the `side-by-side-2-grid` + `concurrent-sync` scenarios (multi-grid Slice 5b) already gate. No new `SMATCHET_UI_PERF_SCOPE` marker needed unless the popup-build measurably costs (it won't — measure if doubted, don't add speculatively).

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

Per `docs/plans/shipped/pillar-1-2-perf-review-system.md`. The diff touches `Source/Core/` (Ui + Commands + Config).

1. **PR-fast CI** — **fires** (reuse existing): `side-by-side-2-grid` + `concurrent-sync` already cover the cross-backend-pane spin-up path this enables; no new scenario needed (the picker only changes *how* a cross-backend pane is created, not the per-frame cost once live). Declare reuse in the PR, don't add a redundant scenario.
2. **Pillar 2 static scanner** — **fires**: assert no new UI-thread sync/HTTP/SQLite in the create path; the bucket-bootstrap read reuses the existing `WarnIfOnUiThread`-guarded loader.
3. **Dispatcher drain** — **N/A**: no change to per-pane apply routing/budget (multi-grid Slice 3 owns that).
4. **Visible-cue bucket-E harness** — **fires conditionally**: the new pane's first-sync cue is the existing per-pane cue; extend bucket-E only if the create-on-backend path surfaces a new stall.
5. **Marker inventory** — **N/A**: no new perf marker planned.

**Pre-push local check**: `docs/guides/perf-workflow.md` § Gate-check vs baseline against `priority-grid-scroll` + `side-by-side-2-grid` (no regression expected — additive UI).

**Override**: `perf-out-of-band` per `AGENTS.md` § Merge gates if an unexpected regression needs a baseline bump (not anticipated).

## Risks / non-goals

- **Split-button vs modal (UX choice)** — a "New pane…" modal (backend combo + view combo + Create) was weighed. Rejected for v1: it adds a click + a modal lifecycle for the common case and the split-button gives the same power inline. Revisit if the popup's two-level menu (backend → view) tests as hard to discover; the `PaneAddRequest` core is UI-agnostic, so swapping the surface later is cheap.
- **Credential gating source (open design decision)** — the picker must know which backends are credentialed to enable/disable entries. **Needs grounding**: confirm whether a per-backend credential check already exists (likely in `ConfigManager` alongside the per-type credential fields) before adding `HasCredentialsFor`. If credentials are only meaningful for the *globally-selected* backend today, this plan must decide: (a) read each backend's stored creds independently (preferred — panes are per-backend), or (b) gate to "backends with a non-empty stored config". Resolve in Slice 2; flagged, not assumed.
- **cfg-follows-focus interaction** — creating a cross-backend pane and focusing it re-points global `cfg.TrackerType` (existing behaviour). This is correct (focused pane drives cfg) but means the **Preferences tracker selector** and the **focused pane's backend** are the same value — a user who creates a GitHub pane and focuses it sees Preferences flip to GitHub. Documented-as-intended (the focused pane *is* the active backend); not a bug. No change to the model.
- **Non-goal: multi-profile (two Jira servers)** — `backendKey` is still one-per-type; the picker lists types, not profiles. `backendKey → profileId` forward-compat is inherited from `multi-grid-tabs.md`, not advanced here.
- **Non-goal: cross-pane aggregation / joined view** — each pane stays one `(backend, view)`.
- **Non-goal: changing the duplicate-by-default `+`** — the bare `+` click stays pure-duplicate; only the caret adds selection. `pane.duplicate` / `pane.split` stay pure-duplicate by name.
- **Non-goal: per-pane catalog VALUE-read** — a freshly-created cross-backend pane shows raw field values until first focus (the bounded, self-healing residue already filed P2 in `docs/self-improvement/categories/debt.md` from multi-grid Slice 4); this plan does not fix it.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps where physically possible.

- **Bucket A (pure-logic ctest, `test-rig`)**: `ResolveNewPaneView` (requested-valid / backend-active / backend-first / bootstrap-empty); `ApplyPaneAddAndCloseRequestsCore` with a target-backend request (new pane gets `targetBackendKey` + resolved view, no snapshot inherit on cross-backend, snapshot **is** inherited when target == source); empty-target = byte-identical to today's duplicate; `pane.new` handler arg→`PaneAddRequest` mapping (Slice 3).
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: Slice 2 — open `+ ▾` → pick a backend → assert a new pane appears bound to that backend; disabled entry for an uncredentialed backend is non-selectable. Run ~4× back-to-back (lane is known-flaky; a new flake is a real gate regression to file).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) on every `Source/Core/`-touching slice.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model (the `PaneAddRequest` target semantics vs the existing duplicate latch; backend-key set vs profileId forward-compat; credential-gating decision; the cfg-follows-focus interaction) before finalising; record the outcome. Required — do not delete.
- **Visual-validation pause (Pillar 4)**: Slice 2 touches `Smatchet*Ui*.cpp` and adds a visible affordance with no bucket-E-until-authored coverage → the orchestrator pauses with the launched exe for user verification (`AGENTS.md` § Autonomous ship-loop default exception 5). Name the deferred-automation action + a `docs/self-improvement/categories/tooling.md` entry if residue remains.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here (multi-profile picker, modal create dialog, per-pane catalog value-read) and revise or delete them.

- **Linear backend in the picker** — auto-included once [`linear-tracker-backend.md`](linear-tracker-backend.md) adds it to the backend-key set; no picker change needed (enum-driven).
- **Multi-profile picker (two same-type backends)** — follow-up; needs `profileId`.
- **Modal create dialog** — weighed + rejected for v1 (§ Risks); revisit only if the split-button menu tests poorly.
- **Per-pane catalog value-read for an unfocused new pane** — inherited P2 debt; not fixed here.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

N/A — design draft, not yet implemented.

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

N/A — design draft, not yet implemented.

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

N/A — design draft, not yet implemented.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped. Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/pane-backend-picker.md docs/plans/shipped/` (move into the shipped tier),*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md`. Write new plan references tier-less.*

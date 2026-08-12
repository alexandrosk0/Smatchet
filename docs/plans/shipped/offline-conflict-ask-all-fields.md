# Plan — Ask the user on offline-replay conflict for all field edits

> **Status**: shipped — archived 2026-06-06; post-ship sections populated and cited PRs merged (see § Implementation log).
>
> **Slug**: `offline-conflict-ask-all-fields` (matches this file's basename without `.md`).
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section. Sections that genuinely don't apply get `N/A — <one-line reason>`, not deletion.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Today the offline-queue replay loop only detects "someone changed the value in between" for **rich-text** field edits. `OfflineQueueService::ResolveFieldEditThreeWayMerge` ([OfflineQueueService.cpp:710](../../../Source/Core/src/Sync/OfflineQueueService.cpp)) captures a base (`OriginalRichValue`) at queue time, re-fetches the server value at replay, and runs `TextMerge::ThreeWayMerge`. On conflict it records `has_merge_conflict` + `conflict_context_json` and suspends the row; the user resolves later in the Offline Queue UI modal (`DrawOfflineConflictModal`, [SmatchetOfflineQueueUi.cpp:1098](../../../Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp)).

Three gaps remain — all **silent last-write-wins** that overwrite a concurrent edit by another user with no consent:

1. **Scalar fields** (status, assignee, priority, labels, dates, etc.) never capture a base, so divergence is never detected — the queued value blind-overwrites the server on replay.
2. **Rich-text fetch failure** — when the server re-fetch fails or returns empty, `ResolveFieldEditThreeWayMerge` logs a WARN and replays the original edit as-is ([OfflineQueueService.cpp:754-762](../../../Source/Core/src/Sync/OfflineQueueService.cpp)), again overwriting whatever the server now holds.
3. Both above also apply when the base was simply never captured (legacy rows, fields introduced before capture).

Intended outcome — **after this lands, no offline field edit overwrites a concurrently-changed server value without first asking the user to decide**, mirroring the existing rich-text conflict flow (async, surfaced in the Offline Queue UI, never a blocking modal mid-replay).

Scope locked via clarifying questions (2026-06-04): **(1)** cover scalar fields **and** the rich-text fetch-failure case; **(2)** when the server value is unverifiable (fetch error / unreachable), suspend + ask rather than silently overwrite; **(3)** async UX mirroring the existing rich-text flow; **(4)** scalar resolution offers Keep mine / Keep theirs / manual-edit value.

Design decisions + rationale recorded in [ADR-0016](../../adr/0016-offline-scalar-edit-conflict-detection.md). Stress-tested via `grill-with-docs` 2026-06-04 — four forks resolved (see § Verification).

## Approach

Generalise the existing rich-text conflict machinery to a **kind-tagged** conflict so the same suspend → surface → resolve loop covers scalar fields and the unverifiable case, without forking a second replay path (Sync subsystem invariant: replay reuses one path).

**Capture.** Add a scalar base alongside the rich base. The UI-side `PendingFieldEdit` gains `OriginalValue` (the pre-edit scalar display value, read from `CachedTicket::GetFieldValue(field.Id)` at edit-commit time — the scalar twin of `OriginalRichValue`). It threads through `AppController::QueueFieldEditOffline` → `OfflineQueueService::QueueFieldEditOffline` → `LocalCacheManager::EnqueuePendingFieldEdit` into a new additive `pending_field_edits.original_value` column. `PendingFieldEditRecord` gains `OriginalValue`.

**Detect (replay).** Scalar detection is **2-way on display values** (grill Q1): the queued `fields_payload_json` is backend-format (account/transition ids, ADF) and not comparable to the `GetFieldValue` display string, so `mine` plays no part in detection — only `base` (display, persisted) vs `theirs` (display, re-fetched). Extract the pure decision into a banned-include-free policy header (`OfflineFieldConflictPolicy.h`, sibling of `OfflineQueueReplayPolicy.h`): `ServerMovedFromBase(base, theirs)` (whitespace-trimmed compare) so `test-rig` exercises it without the SQLite/cpr cascade. In `ReplayOneFieldEdit`: if the field is rich → existing 3-way text merge (unchanged). Else if a scalar base was captured → new `ResolveFieldEditScalarConflict` helper: fetch `theirs = fresh.GetFieldValue(fid)`; if `ServerMovedFromBase` → record a conflict with `conflict_context_json` `{"kind":"scalar", base, mine, theirs, fieldId}` (`mine` sourced from the locally-applied cache value, display only) and suspend.

**Fetch-failure / unverifiable (grill Q3, decision (c)).** For **both** rich and scalar, when the pre-replay server re-fetch fails AND a base was captured: a *transient* (transport) error retries on the next tick (bump attempts), and when it would reach the attempt cap it routes to a `{"kind":"unverified"}` conflict **instead of** the dead-letter; a *permanent* error (404/403, via the existing `IsTrackerTransportErrorText` classifier) raises the `unverified` conflict immediately. This replaces the current rich-text "replay as-is on fetch fail" (gap 2) and establishes the contract: **a field edit that captured a base never dies silently — it replays or asks.** Rows with no base captured keep today's behaviour (can't ask about a change we never referenced) — a known residue, not a silent overwrite of a *detected* change.

**Resolve (UI).** `DrawOfflineConflictModal` branches on the context `kind`: absent/`text` (legacy rich) → today's markdown 3-pane + conflict-marker editor (unchanged); `scalar` → a compact pane showing mine vs theirs with **Use Mine / Use Theirs / editable value**; `unverified` → mine value + "server value couldn't be read" with **Force Mine / Discard my edit**. `AppController::ResolveFieldEditConflict` is generalised to carry the kind: scalar/unverified resolutions write the chosen value into the payload key directly (no markdown→ADF/HTML conversion), and "Discard" hard-deletes via `DeletePendingFieldEdits([id])` **plus a `BackendAuditTrail` discard entry** (grill Q4 — dead-letter bucket stays failures-only; traceability lives in the append-only audit log). Resolve NULLs **both** `original_rich_value` and `original_value` so the next replay performs the now-consented overwrite. The context `kind` discriminator is `text` | `scalar` | `unverified`; rows written before this change carry no `kind` and default to `text`, keeping the existing rich-text modal byte-identical. The new `kind` is **orthogonal to the existing per-field `richKind`** key (`adf`|`html`, written at [OfflineQueueService.cpp:788](../../../Source/Core/src/Sync/OfflineQueueService.cpp) and consumed by the resolve helper's `richKind` param): `kind` selects the conflict *category* (text/scalar/unverified), while `richKind` is **unchanged** and continues to drive the rich-branch markdown→ADF/HTML reconversion at resolve. New rich conflicts write `kind:"text"` explicitly alongside the existing `richKind`; the rich context shape becomes `{kind:"text", base, mine, theirs, fieldId, richKind}`.

Trade-off: a separate `original_value` column rather than overloading `original_rich_value` — the replay branches on rich-vs-scalar and conflating the two bases would make the isAdf detection ambiguous. Additive column is cheap; reuse is not.

## Files to modify

Capture path:
1. [SmatchetGridFieldEditPipeline.cpp:35](../../../Source/Core/src/SmatchetGridFieldEditPipeline.cpp) — pass `edit.OriginalValue` to `QueueFieldEditOffline`.
2. [SmatchetUiSession.h:82](../../../Source/Core/include/Ui/SmatchetUiSession.h) — add `OriginalValue` to the `PendingFieldEdit` struct (next to `OriginalRichValue`).
3. `Source/Core/src/TicketFieldEditor*.cpp` / `TicketFieldEditor_Modal.cpp:437` — set `edit.OriginalValue = ticket.GetFieldValue(field.Id)` at edit-open/commit (mirror the `OriginalRichValue` capture).
4. [AppController.h:780](../../../Source/Core/include/AppController.h) + [AppController_IssueCreateOffline.cpp:199](../../../Source/Core/src/AppController_IssueCreateOffline.cpp) — add `originalValue` param to `QueueFieldEditOffline` (default `std::string()` for back-compat).
5. [OfflineQueueService.h:95](../../../Source/Core/include/Sync/OfflineQueueService.h) + [OfflineQueueService.cpp:419](../../../Source/Core/src/Sync/OfflineQueueService.cpp) — thread `originalValue` through.

Persistence (additive schema):
6. [LocalCacheManager.cpp:85](../../../Source/Core/src/Persistence/LocalCacheManager.cpp) — `ALTER TABLE pending_field_edits ADD COLUMN original_value TEXT` + the dead-table twin (additive only — Persistence invariant).
7. [LocalCacheManager.cpp:641](../../../Source/Core/src/Persistence/LocalCacheManager.cpp) `EnqueuePendingFieldEdit` + [:668](../../../Source/Core/src/Persistence/LocalCacheManager.cpp) `LoadPendingFieldEdits` — bind/read `original_value`.
8. [LocalCacheManager.cpp:724](../../../Source/Core/src/Persistence/LocalCacheManager.cpp) `ResolveFieldEditConflict` — also `original_value = NULL`.
9. [LocalCacheManager.h:78](../../../Source/Core/include/Persistence/LocalCacheManager.h) — signature update.
10. [CachedTicketTypes.h:63](../../../Source/Core/include/CachedTicketTypes.h) — add `OriginalValue` to `PendingFieldEditRecord`.

Detect (replay):
11. **NEW** `Source/Core/include/OfflineFieldConflictPolicy.h` — pure `ServerMovedFromBase(base, theirs) -> bool` (whitespace-trimmed 2-way compare; zero banned includes, sibling of `OfflineQueueReplayPolicy.h`).
12. [OfflineQueueService.h:170](../../../Source/Core/include/Sync/OfflineQueueService.h) + [OfflineQueueService.cpp:794](../../../Source/Core/src/Sync/OfflineQueueService.cpp) — add `ResolveFieldEditScalarConflict`; change the rich-path fetch-fail branch + add the scalar fetch-fail branch to follow decision (c) (transient→retry, at-cap→`unverified` conflict, permanent→`unverified` now) instead of replaying as-is; `ReplayOneFieldEdit` dispatches rich vs scalar. Reuse the existing `IsTrackerTransportErrorText` classifier for the transient/permanent split.

Resolve (UI):
13. [SmatchetOfflineQueueUi.cpp:1098](../../../Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp) `DrawOfflineConflictModal` — branch on `kind` (`text` (legacy/absent) | scalar | unverified); add scalar + unverified panes with Use Mine / Use Theirs / edit / Discard. The rich `text` branch keeps using the existing orthogonal `richKind` (adf|html) for reconversion — do not conflate the two keys.
14. [AppController_IssueCreateOffline.cpp](../../../Source/Core/src/AppController_IssueCreateOffline.cpp) + [OfflineQueueService.cpp:486](../../../Source/Core/src/Sync/OfflineQueueService.cpp) `ResolveFieldEditConflict` — carry `kind`; scalar/unverified write the value into the payload key directly; "Discard" → `DeletePendingFieldEdits`.

Tests:
15. **NEW** `tests/Core/OfflineFieldConflictPolicy.test.cpp` — table-drive the classifier.
16. [tests/Core/OfflineQueueServiceRuntime.test.cpp](../../../tests/Core/OfflineQueueServiceRuntime.test.cpp) — scalar-conflict + unverified-suspend replay cases via `FakeOfflineQueueDeps`.

## Existing utilities reused

- `CachedTicket::GetFieldValue` — [CachedTicketTypes.h:23](../../../Source/Core/include/CachedTicketTypes.h) — scalar "theirs"/base value source (twin of `GetFieldRichValue`).
- `LocalCacheManager::MarkFieldEditConflict` / `ResolveFieldEditConflict` — [LocalCacheManager.cpp:710](../../../Source/Core/src/Persistence/LocalCacheManager.cpp) — reuse the existing conflict-record/clear plumbing; only the context payload + base-NULLing change.
- `OfflineQueueService::RunFieldEditCacheMutation` — [OfflineQueueService.cpp:~690](../../../Source/Core/src/Sync/OfflineQueueService.cpp) — wrap the new conflict-record cache write.
- `DeletePendingFieldEdits` — [OfflineQueueService.h:106](../../../Source/Core/include/Sync/OfflineQueueService.h) — the "Discard my edit" resolution.
- `OfflineQueueReplayPolicy.h` — pattern template for the new pure `OfflineFieldConflictPolicy.h`.
- `BackendAuditTrail::AppendResult` — record each conflict decision (Sync invariant: conflict detection records its decision).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — detection runs in the off-UI-thread replay task; the modal is on-demand, not a steady-state render path.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no new UI-thread I/O — server re-fetch already happens off-thread inside `Tick*`; the resolve modal does only an in-memory cache write (consistent with today's rich-text resolve).
- **Pillar 3 (never crash)**: all new JSON parse / cache writes go through existing try/catch wrappers (`RunFieldEditCacheMutation`); classifier is pure + bounds-free. Malformed `conflict_context_json` falls back to a safe empty render (mirror [:1118](../../../Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp)).
- **Pillar 4 (accessibility)**: scalar pane uses standard ImGui widgets (keyboard-navigable); no new contrast concerns vs the existing modal. Aspirational — no auto-gate.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`)

1. **PR-fast CI** — closest scenario is the offline-queue-panel render path; declare the offline-queue scenario from `agents/core/perf-gatekeeper.md` § Curated diff → scenario map (or `N/A — modal is on-demand, replay is off-thread` if no offline scenario is curated). Confirm against `scripts/dev/perf-pr-fast-set.json` at implementation.
2. **Pillar 2 static scanner** — no new sync-I/O reachable from `ImGui::*` (re-fetch stays in the existing off-thread `Tick*` task). No annotation needed.
3. **Dispatcher drain** — does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — no new sync-stall code path > 100 ms.
5. **Marker inventory** — adds no `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check if the offline-queue scenario is curated; else note N/A in the PR.

**Override**: not anticipated.

## Risks / non-goals

- **Risk — false-positive scalar conflicts from serialization drift.** Mitigated structurally: `base` and `theirs` both come from the **same** `GetFieldValue` serializer, so an unchanged field yields `base == theirs` exactly. A false positive only arises if the server re-serializes without a semantic change (e.g. label re-ordering) — uncommon, and arguably a real change. v1 applies a whitespace trim on both sides (grill Q2); per-field semantic normalization (label-set union) is deferred to § Out of scope. Where unsure, prefer a conflict (ask) over a silent overwrite — the locked stance; a false positive costs one dismissable ask, never data loss.
- **Risk — base never captured on legacy rows** → still last-write-wins. Accepted: we can't ask about a divergence we have no reference for; only *detected* changes are gated. Called out in § Out of scope.
- **Non-goal — optimistic concurrency at the HTTP layer** (If-Match/etag/version). Confirmed absent today (`rg` over `Source/Core/src/Tracker` → 0 hits). This plan stays at the offline-queue base/theirs comparison level; a true server-version guard is a separate, larger effort.
- **Non-goal — create-replay conflicts.** A create makes a new issue; there is no in-between value. Unchanged.
- **Non-goal — dead-letter field edits.** No restore path exists for dead field edits (only delete), so they are never replayed — out of scope by construction. Dead *creates* restore as fresh creates (no value conflict).

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `OfflineFieldConflictPolicy::ServerMovedFromBase` table — `base==theirs` → false (server unchanged → replay); `base!=theirs` → true (ask); whitespace-only difference → false (trim); empty base → false (no detection, residue). New `OfflineFieldConflictPolicy.test.cpp`.
- **Bucket A — runtime**: `OfflineQueueServiceRuntime.test.cpp` via `FakeOfflineQueueDeps` — scalar divergence records a `kind:scalar` conflict + suspends (not replayed); permanent fetch-failure with base captured records `kind:unverified` + suspends; transient fetch-failure retries then routes to `unverified` at the attempt cap (decision (c) — never silent dead-letter); no-base row still replays (documented residue); resolve clears both bases; discard hard-deletes + emits the audit entry.
- **Bucket E (ImGui Test Engine)**: drive `DrawOfflineConflictModal` for a `scalar` and an `unverified` context — assert Use Mine / Use Theirs / edit / Discard render and each routes to the right `ResolveFieldEditConflict`/`DeletePendingFieldEdits` call.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target — `Source/Core` change).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: **run 2026-06-04.** Four forks resolved: (Q1) scalar detection is 2-way display-value `ServerMovedFromBase`, not the broken `(base, mine=payload, theirs)` 3-way; (Q2) all scalar fields, whitespace-trim, semantic normalization deferred; (Q3) decision (c) — a based edit never dies silently; (Q4) discard hard-deletes + audit entry. Storage substrate verified against [LocalCacheManager.cpp:646](../../../Source/Core/src/Persistence/LocalCacheManager.cpp) (additive `ALTER TABLE` pattern). Decisions recorded in [ADR-0016](../../adr/0016-offline-scalar-edit-conflict-detection.md).
- **Manual residue**: none expected; bucket-E covers the modal. If the ImGui-test glue for the scalar pane proves infeasible this round, file a `docs/self-improvement/categories/tooling.md` entry with the deferred-automation plan — no silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here.

- **Legacy rows with no captured base** — still last-write-wins on replay. Follow-up: a one-time "capture base on next edit" is automatic; no migration backfills bases for already-queued rows. No-action.
- **Per-field semantic merge for multi-value scalars** (e.g. union of label sets) — the manual-edit value covers it for now; an auto-union is a follow-up.
- **HTTP-layer version guard (If-Match/etag)** — separate plan if pursued.

## Implementation log

Shipped as **PR #854** (single feature PR, merged 2026-06-05) by `offline-sync`. Four workstreams landed as planned: capture+persist (additive `original_value` column + dead-table twin, threaded `OriginalValue` end-to-end), detect+replay (`OfflineFieldConflictPolicy.h` + scalar dispatch + decision-(c) fetch-fail routing), resolve UI (`DrawOfflineConflictModal` branches on `kind`, scalar + unverified panes), and tests (bucket-A policy table + 8 runtime `FakeOfflineQueueDeps` cases). The corrected `kind` (text|scalar|unverified) vs existing `richKind` (adf|html) contract from the triple-check (#853) held — new rich conflicts write `kind:"text"` alongside the untouched `richKind`.

A CodeRabbit triage round (8 findings, 6 substantive) landed before merge — see § Deviations for the design-affecting ones.

## Deviations from plan

1. **Single re-fetch hoisted** — the plan kept the re-fetch inside `ResolveFieldEditThreeWayMerge`; implementation extracted `EvaluateFieldEditConflict` so rich + scalar share **one** fetch and the decision-(c) routing lives in one place. Same observable contract, no second fetch.
2. **Presence flag instead of empty-string sentinel** (CR Major) — the plan keyed scalar detection on `!OriginalValue.empty()`, which conflates a genuinely *blank* captured base with a legacy no-base row (silent overwrite for blank-field edits). Fixed with an additive `has_original_value` column + `HasOriginalValue` flag + a presence-aware `ServerMovedFromCapturedBase(hasBase, …)` entry point; the emptiness-keyed `ServerMovedFromBase` stays for callers without a flag. Rich-side `OriginalRichValue.empty()` left as **pre-existing residue** (parallel `has_original_rich_value` deferred — non-trivial, not this PR's contract).
3. **Non-destructive malformed-context pane** (CR Critical) — plan said "safe empty render"; CR required it be non-*actionable* too. Implemented `ConflictModalCtx::Valid` + `DrawConflictPaneUnknown` (read-only, Close + hard Discard only) so a corrupt row can't be "resolved" to empty content.
4. **No fabricated fallback payload** (CR Critical) — the resolve path's load/find-failure branch originally wrote a `__resolved__`-sentinel-keyed payload (lost the real field key → misapply on replay); changed to log-and-skip.
5. **Audit begin/result pair** (CR Major) — the new conflict-suspend paths emit the full `FieldEditAuditSource`-attributed begin/result pair per the Sync leaf-`AGENTS.md` invariant (the plan's § Existing utilities only said "record the decision").
6. **Server-side clear is now a conflict** (CR Major) — `theirsRich == ""` (concurrent delete) enters the merge instead of silently overwriting.
7. `ReplayOneFieldEdit` ended at 107 lines — soft func-size WARN (non-blocking, under the 120 hard cap); left readable.

## Verification (actual)

- **Bucket A**: new `tests/Core/OfflineFieldConflictPolicy.test.cpp` (presence + emptiness classifier tables) + 8 new `OfflineQueueServiceRuntime.test.cpp` runtime cases (scalar conflict suspends, permanent/transient fetch-fail → unverified, blank-captured-base IS conflict-checked, no-fabrication on missing row, resolve clears both bases, discard hard-deletes + audit entry). `ninja-test-msvc` → **ctest 100% passed / 0 failed** (initial 1256/0; re-verified after the CR round).
- **Dual-target build**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` → **PASS** (/WX warning-clean) on both the initial impl and the CR-fix head. Light config (WHISPER/AI/MCP off) → PASS.
- **Pre-ship delta gate** (`scripts/dev/pre-ship.sh`) → PASS (advisory WARNs only). One follow-up comment-noise fix (blank `//` / `///` separators in `OfflineFieldConflictPolicy.h`) was caught by the CI delta gate post-push and healed.
- **Bucket E**: deferred — no offline-queue ImGui-Test fixture exists; logged in `docs/self-improvement/categories/tooling.md` (§ Parked) with the deferred-automation plan. Service-level safety (no fabrication, non-actionable malformed) is bucket-A-covered.
- **Sanitizer (ASAN)** + Bucket-C/E + Perf PR-fast → all green at merge.
- **Manual residue**: live in-app smoke (network-off → edit → network-on → confirm replay/ask) not run interactively; the async loop is covered by the bucket-A runtime harness. Rich-side empty-base presence-awareness is the one carried-forward residue (deviation 2).

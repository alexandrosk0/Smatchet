# Plan — AppController fan-in Phase 6: dissolution to the composition-root ceiling

> **Slug**: `appcontroller-fan-in-phase6-dissolution`
>
> **Status**: `active` — audit-driven execution. Successor to `docs/plans/shipped/appcontroller-fan-in-phase5-facets.md` (8 facets, −10).

<!-- index-summary: Phase 6 of the AppController fan-in reduction — a full audit of all 111 AppController.h includers classified every file into tiers; this plan executes the tiers in order (free include-drops, poster-upcast fixes, nested-type hoists, methods-only facets, mainThreadDispatcher de-publicizing, IScenario narrowing) down to the composition-root ceiling (~34 includers). -->

## Context

Phase 5 exhausted the *cleanly-migratable command-TU cluster* (−10). External feedback proposed dissolving AppController along the proven `IApp*` seam by auditing every includer. The audit was run (2026-07-06): all **111** direct `#include "AppController.h"` sites classified by what they actually use (method calls with receiver matching, inline-getter intersection, public-member reach-ins, nested-type references, ownership signals, pass-through-only detection).

## Audit result (the work-list)

| Tier | Files | Mechanism | Δ ceiling |
|---|---|---|---|
| Terminal: implementation TUs | 12 | the class itself | 0 |
| Terminal: composition roots | 4 | own/construct/tick the concrete object | 0 |
| Terminal: dispatcher orchestrator | 1 | deviation-documented | 0 |
| T1a: zero-deref scenario TUs | 15 | delete include (`IScenario.h` fwd-declares; override signatures need no completeness) | −15 |
| T1b: vestigial TUs | 4 | delete include (fwd-decl if signatures name AppController&) | −4 |
| T1c: poster-upcast-only command TUs | 2 | two-param registrar (`IMainThreadPoster&`); `SubmitBugReport(app,…)` pass-through is fwd-decl-safe | −2 |
| T2: nested-type dependents | 7 | hoist `AppController::{AttachmentDescriptor, FieldEditResult, TrackerConnectivityState, *DeleteSummary/*RestoreSummary}` to rank-0 `Types/`; `Ui/SmatchetUiSession.h` is the transitive-spread kill | −7 |
| T3: methods-only | 21 | existing + new narrow facets (URL, pane-focus, `Commands()` registry, MCP-activity, Lua-console) | −21 |
| T4: `mainThreadDispatcher` reach-ins | 17 | route posts through `IMainThreadPoster::PostToMainThread`; queue-metric accessors for Perf | −17 |
| T5: scenario TUs using app | 11 | narrow `IScenario`'s `AppController&` signature to a scenario-host facet | −11 |
| T6: Pillar-1 inline-getter consumers | 15 | **blocked** on a non-virtual read-model design (revision-gated snapshot) — attempted last; documented terminal if unsafe | −15 (conditional) |

Ceiling: 111 → **~34** (or ~19 if T6 lands).

## Verification

No Core TU compiles locally (Linux container; FetchContent egress-blocked) — **CI is the correctness gate** per slice: MSVC full/light/ARM64, POSIX core gate, sanitizers, Bucket-C/E. Local pre-push gates each slice: fan-in ratchet `--diff`, include-cycle/layer-rank, duplication delta, comment-noise, lint.

## Risks / rules (carried from Phase 5)

- Never virtualize an inlined per-frame getter (Pillar-1).
- Drop a concrete override's default arg only when no concrete-typed caller relies on it; otherwise mirror.
- A facet header is rank-0: forward-declare higher-rank types (validated for by-value returns, `std::vector<incomplete>&` params, `std::future<incomplete>` returns).
- Include-deletion slices may surface transitive-include breakage (types that arrived via AppController.h); fix by adding the direct include CI names.

## Implementation log

*(appended per merged slice)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip § Status to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.

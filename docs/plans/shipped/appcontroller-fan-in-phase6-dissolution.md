# Plan — AppController fan-in Phase 6: dissolution to the composition-root ceiling
<!-- plan-date: 2026-07-10 -->

> **Slug**: `appcontroller-fan-in-phase6-dissolution`
>
> **Status**: `shipped` — executed 2026-07-10 (five slices, fan-in 111 → 77). Successor to `docs/plans/shipped/appcontroller-fan-in-phase5-facets.md` (8 facets, −10).

<!-- index-summary: Phase 6 of the AppController fan-in reduction — a full audit of all 111 AppController.h includers classified every file into tiers; five shipped slices (free include-drops, poster registrars, the UiSession.h nested-type hoist, and the IAppCommands / IAppThreading / IAppScenarioHost facets) brought fan-in from 111 to 77, the documented terminal set. -->

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
| T3: methods-only | 22 | existing + new narrow facets (URL, pane-focus, `Commands()` registry, MCP-activity, Lua-console, AI-assistant) — incl. `SmatchetAiAssistantUi.cpp`, initially misread as a composition root (comment-text false positive) | −22 |
| T4: `mainThreadDispatcher` reach-ins | 18 | route posts through `IMainThreadPoster::PostToMainThread`; queue-metric accessors for Perf — incl. `SmatchetFieldIconRender.cpp` (`app.mainThreadDispatcher.PostToMainThread` in its texture workers), initially misread as a composition root | −18 |
| T5: scenario TUs using app | 11 | narrow `IScenario`'s `AppController&` signature to a scenario-host facet | −11 |
| T6: Pillar-1 inline-getter consumers | 15 | **blocked** on a non-virtual read-model design (revision-gated snapshot) — attempted last; documented terminal if unsafe | −15 (conditional) |

Ceiling: 12+4+1 terminal +15+4+2+7+22+18+11+15 tiers = **111** (reconciled — two comment-text false positives originally dropped from the counts are now in T3/T4). The audit's optimistic ceiling was ~32 (or ~17 with T6); execution landed at **77** — see the Implementation log for the honest per-tier deltas and why the chain-coupled T3/T4 remainder and the Pillar-1 T6 set are terminal.

## Verification

No Core TU compiles locally (Linux container; FetchContent egress-blocked) — **CI is the correctness gate** per slice: MSVC full/light/ARM64, POSIX core gate, sanitizers, Bucket-C/E. Local pre-push gates each slice: fan-in ratchet `--diff`, include-cycle/layer-rank, duplication delta, comment-noise, lint.

## Risks / rules (carried from Phase 5)

- Never virtualize an inlined per-frame getter (Pillar-1).
- Drop a concrete override's default arg only when no concrete-typed caller relies on it; otherwise mirror.
- A facet header is rank-0: forward-declare higher-rank types (validated for by-value returns, `std::vector<incomplete>&` params, `std::future<incomplete>` returns).
- Include-deletion slices may surface transitive-include breakage (types that arrived via AppController.h); fix by adding the direct include CI names.

## Implementation log

- **T1 — free include-drops (#1701, merged 7cef1efc): −21, 111 → 90.** 15 zero-deref scenario TUs deleted the include outright (`IScenario.h` forward-declared the type); 4 vestigial TUs swapped include → forward declaration; the bug-report and UI-interaction command registrars were retyped to take `IMainThreadPoster&` alongside the concrete reference, with `SubmitBugReport(app, …)` staying a forward-decl-safe pass-through. Side fix: `test-mutation-smoke-bats.sh` added because develop's orphan-bats gate was red on a suite that landed without a wrapper.
- **T2 — nested-type hoist / transitive kill (#1707, merged c145edb7): −1 direct, 90 → 89, plus the spread-stopper.** `Ui/SmatchetUiSession.h` stopped including AppController.h by re-spelling members against the already-hoisted `Types/`/`Sync/` leaf headers. Fix-forward: three command TUs that had leaned on the transitive include were retyped to `IMainThreadPoster&`.
- **T3a — IAppCommands facet (#1714, merged d3526299): −2, 89 → 87.** New `Interfaces/IAppCommands.h` (the `Commands()` registry accessor pair); `PaneCommands` and the keybindings preferences tab chain retyped to it. The rest of the methods-only tier was reclassified **chain-coupled-deferred** during execution: most "methods-only" files pass `AppController&` through helper signatures, store it in context structs, or route it via `IPlugin`, so single-file swaps do not compile — only chains of ≤2 local files were taken.
- **T4 — IAppThreading facet + dispatcher de-publicizing (#1717, merged ad2b871c): −5, 87 → 82.** New `Commands/IAppThreading.h` (extends `IMainThreadPoster` with `LaunchBackgroundTask`); AppController's poster base swapped to it, so every existing upcast kept working. All 34 non-implementation `app.mainThreadDispatcher.PostToMainThread(...)` reach-throughs became facet calls. Five TUs whose only dependency was the worker/post-back idiom dropped the include (ModelDownloader, the Whisper preferences tab, the long-text editor modal, AiPrefsTestConnection, MergeWatchNotifyServer — the last takes plain `IMainThreadPoster&`). The member stays public solely for the composition tick loops' `Drain()` and the Perf command's queue metrics.
- **T5 — IAppScenarioHost facet (#1720, merged eb9180e9): −5 net, 82 → 77.** New rank-0 `Interfaces/IAppScenarioHost.h` (`EnsurePaneContextLive` + the ScenarioLua hooks); the `IScenario` lifecycle, `ScenarioRunner::Tick`, and the runner's cancel stash retyped to it; ~27 scenario TUs re-signed. Six scenarios dropped the include (two obsolete "no narrower interface exposes it" deviations removed with them). Five scenarios that wire `CommandContext::App` keep the concrete type and recover it through the single `RequireConcreteController` seam (declared in IScenario.h, defined in ScenarioRunner.cpp — which became the +1 deviation-documented orchestrator includer, mirroring the BuiltinCommands dispatcher). The audit's −11 estimate was optimistic: `ctx.App` wiring in three more scenarios than predicted forced the keep-concrete set.
- **T6 — verdict: terminal by design (no code).** All 15 category-G consumers reach at least one Pillar-1 inline per-frame getter (`GetAvailableFields` ×9, the revision/error getters, live backend/plugin-host handles). Virtualizing is forbidden; the alternative — a non-virtual revision-gated snapshot read-model — would thread a new type through every Ui draw chain (~25–30 files) for at most ~5 include drops, since most G files also use non-snapshot surface and keep the include anyway. Fails the behavior-preserving-thrift bar; revisit only if a Ui read-model lands for product reasons.
- **Terminal set at 77**: 12 implementation TUs, the composition roots (Standalone, Android, SmatchetUI, AppController_Init), 2 deviation-documented orchestrators (BuiltinCommands dispatcher, ScenarioRunner), the chain-coupled Ui/command cluster (draw-signature chains, `CommandContext::App`, `IPlugin` routing, `BugReportService`-style free-function seams), and the 15 Pillar-1 inline-getter consumers.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip § Status to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.

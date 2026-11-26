# Plan — Reopen: narrow what UI panels see of AppController

> **Slug**: `ui-panels-iapp-facets-reopen` (matches this file's basename without `.md`).
>
> **Status**: `active` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section. Sections that genuinely don't apply get `N/A — <one-line reason>`, not deletion — the headings drive the "did you consider this?" forcing function for every author + reviewer agent.
>
> **Plan-link convention (tier-less — write ALL plan references this way)**: link other plans by the **tier-less** form `docs/plans/<slug>.md` — NOT `docs/plans/active/<slug>.md` or `docs/plans/shipped/<slug>.md`. The link gates (`test-markdown-links`) resolve a tier-less path against `active/`, `shipped/`, or `deferred/`, so the reference keeps working when a plan is archived (`git mv active → shipped`) without any ref-sweep. A **tiered** link passes the local check while the target still lives in `active/`, then 404s on CI after the archival move — the `markdown-links-local-passes-ci-fails-after-plan-archive` trap; `test-markdown-links.sh --merge-tree-warn` flags tiered links before they rot.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

**DRAFT — awaiting the owner's accept or reject. No implementation is authorised by this plan.**

Per the task brief, the orchestrator earlier recommended that UI panels depend on narrow interfaces instead of the whole [`AppController.h`](../../../Source/Core/include/AppController.h), before reading the project's records (this is not checkable in the tree). The records show the project already weighed this and closed it. This plan re-measures the tree and states whether anything justifies reopening.

### Recorded decision

- [`appcontroller-fan-in.md`](../appcontroller-fan-in.md) § Approach: facet carving "has the highest ceiling but is the **only** lever that edits includers and risks per-frame virtual-call cost, so it is **deferred**".
- [`appcontroller-fan-in.md`](../appcontroller-fan-in.md) § UX Pillar callouts → Pillar 1: "Phase 5 must exclude **all per-frame inline getters** from any facet (keep them on the concrete class)". It names ten: `GetActiveTicketsRevision`, `GetFieldCatalogRevision`, `GetAvailableFields`, `GetTrackerBackend`, `GetLastTrackerConnectivityState`, `GetAvailableComponents`, `GetAvailableUsers`, `GetFieldCatalogError`, `GetFieldCatalogWarning`, `GetLastTicketSyncWarning`.
- [`appcontroller-fan-in-phase5-facets.md`](../appcontroller-fan-in-phase5-facets.md) § Approach: "**No facet may contain any of them** … They stay on the concrete class and their callers (per-frame UI TUs) keep `AppController&` — those TUs are intentionally **out of scope** for migration."
- [`appcontroller-fan-in-phase5-facets.md`](../appcontroller-fan-in-phase5-facets.md) § Risks / non-goals: "**Non-goal: migrating the per-frame UI TUs** (`SmatchetActiveProjectGridUi.cpp`, `SmatchetUI.cpp`) — they keep `AppController&` by design."
- [`appcontroller-fan-in-phase6-dissolution.md`](../appcontroller-fan-in-phase6-dissolution.md) § Risks / rules: "Never virtualize an inlined per-frame getter (Pillar-1)."
- [`appcontroller-fan-in-phase6-dissolution.md`](../appcontroller-fan-in-phase6-dissolution.md) § Implementation log, T6: "**verdict: terminal by design (no code).** … the alternative — a non-virtual revision-gated snapshot read-model — would thread a new type through every Ui draw chain (~25–30 files) for at most ~5 include drops, since most G files also use non-snapshot surface and keep the include anyway. Fails the behavior-preserving-thrift bar; revisit only if a Ui read-model lands for product reasons."
- Same section: "**Terminal set at 77**: … the chain-coupled Ui/command cluster (draw-signature chains, `CommandContext::App`, `IPlugin` routing, `BugReportService`-style free-function seams), and the 15 Pillar-1 inline-getter consumers." T3 adds that most methods-only files "pass `AppController&` through helper signatures, store it in context structs, or route it via `IPlugin`, so single-file swaps do not compile".
- [`agent-layer/AGENTS.md`](../../../agent-layer/AGENTS.md) § Enforcement contract-card, `app-controller-fan-in` row: "cap = baseline fan-in (currently 70; enforced as a merge-base delta, not the constant), **ratchet-down only**, **hard-FAIL absolute**".

### What this plan asks

Is there new evidence that clears the bar the T6 verdict set? The measured answer is no, so this plan recommends rejecting the reopen.

## Approach

### Measurements (this tree, 2026-11-25)

| Figure | Value | How measured |
|---|---|---|
| `AppController.h` includers, `Source/**` | **71** (70 `.cpp`, 1 `.h`) | `appcontroller_fan_in_audit.py --list` and the audit summary |
| Same, on `origin/develop` | 73 | `git grep` of the include line at `origin/develop` |
| Ratchet vs `origin/develop` | passes | `--diff origin/develop`, rc 0 |
| Documented baseline constant | 70 | `BASELINE_FAN_IN` in the audit script; AGENTS.md row |
| Includers under `Source/Core/{src,include}/Ui/` | **32** (all `.cpp` under `src/Ui/`) | `--list` filtered; 32 on `origin/develop` too |
| `AppController.h` lines / `#include` lines | **2,181** / **60** | `wc -l`; grep of `#include` |
| First-party headers in its include closure | 55 (plus 27 distinct angle-bracket headers) | scratch closure script over quote-includes, resolved against `Source/Core/{include,src}` and the including file's directory |
| Ui TUs that already use a facet and not the full header | 8 | grep of `Interfaces/IApp` under `Ui/` |

Header moves on this branch (Markdown converter, saved-views store, perf monitor, `TrackerError.h`) did not change the Ui includer count. The two Sync service TUs that dropped the include are outside `Ui/`. The tree-wide count moved 77 (Phase 6 terminal) → 73 (`origin/develop`) → 71 (this branch).

Facets used by Ui today, one include each: `IAppTicketData` (`AnnotateAnalysisUi_Config.cpp`), `IAppTicketMutations` (`AnnotateAnalysisUi_Preferences.cpp`), `IAppMeta` (`SmatchetAboutUi.cpp`, `SmatchetBugReportUi.cpp`, `SmatchetPreferencesUi_General.cpp`), `IAppUsers` (`SmatchetAutocompleteUi.cpp`), `IAppPendingActions` (`SmatchetOfflineQueueUi_Actions.cpp`), `IAppSync` (`SmatchetPreferencesUi_General.cpp`), `IAppCommands` (`SmatchetPreferencesUi_Keybindings.cpp`).

State of the ten named getters today:

- Still inline and on no facet: `GetActiveTicketsRevision`, `GetFieldCatalogRevision`, `GetAvailableComponents`, `GetAvailableUsers`, `GetFieldCatalogWarning`, `GetTrackerBackend`.
- Already declared virtual on a facet: `GetAvailableFields` (`IAppFields`, still header-inlined on the class), `GetFieldCatalogError` (`IAppFields`, `IAppSync`), `GetLastTicketSyncWarning` and `GetLastTrackerConnectivityState` (`IAppSync`; the latter also `IAppTicketMutations`). The last two are out-of-line in `AppController_Connectivity.cpp`.
- The `IAppSync.h` and `IAppFields.h` header comments record why: per-frame callers bind a concrete `AppController&`, so they keep static binding, and an out-of-line accessor costs "at most one vtable lookup/frame". The headers present this as a refinement of the Phase 5 rule (out-of-line or non-draw-loop getters only). Read literally, the Phase 5 text above ("No facet may contain any of them") no longer holds for four of the ten; no record found here amends it explicitly.

Ui includers by text match on those ten names (a proxy: it misses calls through helpers):

- **18 reach at least one**: `AnnotateAnalysisUi_Modals`, `AnnotateAnalysisUi_Window`, `SmatchetActiveProjectGridTable`, `SmatchetActiveProjectGridUi`, `SmatchetBulkTicketsUi`, `SmatchetCommentsModalUi`, `SmatchetGridHeaderUi`, `SmatchetGridSearchUi`, `SmatchetGridUiSupport`, `SmatchetNewIssueDraftUi`, `SmatchetPreferencesUi`, `SmatchetProjectPicker`, `SmatchetQuickCreateIssueUi`, `SmatchetStatusBarUi`, `SmatchetToolbarUi`, `SmatchetUI`, `SmatchetViewsDashboardUi`, `SmatchetViewsDashboardUi_widgets`.
- **14 reach none**: `SmatchetActiveProjectGridCells`, `SmatchetAiAssistantUi`, `SmatchetAttachmentPreviewUi`, `SmatchetBulkFieldEditPopup`, `SmatchetFieldIconRender`, `SmatchetGridPaneWindows`, `SmatchetImGuiHost`, `SmatchetMcpServerUi`, `SmatchetMobileShellUi`, `SmatchetOfflineQueueUi`, `SmatchetPreferencesUi_Assistant`, `SmatchetUI_MainMenu`, `SmatchetUserInfoUi`, `SmatchetUserInfoUi_Sections`.
- Three of the 18 reach only the out-of-line connectivity getter: `SmatchetCommentsModalUi`, `SmatchetProjectPicker`, `SmatchetStatusBarUi`. Each also calls methods on no facet (`FocusedCacheBackendKey`, `RequestTrackerProbeNow`, `UpdateCachedCommentsFromThread`, `BackendShared`, `LookupCacheShared`, `GetOfflineQueueSnapshot`). `SmatchetStatusBarUi.cpp` also names `AppController::TrackerConnectivityState`, and its header signature takes `AppController&`.

### Options

**(a) Do nothing.** Cost: zero. Benefit: zero. The ratchet keeps the count from growing. Matches the recorded verdict.

**(b) Non-virtual per-frame read-model.** A struct of `shared_ptr`-to-const snapshots, built once per frame by the composition root and passed into draw code. Pieces exist: `GetAvailableFields` already returns `std::shared_ptr<const std::vector<TrackerField>>`, and `IAppTicketData::GetActiveTicketsSnapshot` exists. Cost: the record estimated ~25–30 files for ~5 include drops. Today 18 Ui TUs reach the getters, so the threading cost is at least that order. The record's reason for few drops still holds: the three cheapest candidates above all call surface no facet carries. Perf: building snapshots each frame adds refcount bumps and, for `GetAvailableFields`, an uncontended lock per call (the `IAppFields.h` comment); a revision gate would bound it, but nothing is measured here. No Ui read-model has landed for product reasons (grep for `read-model` / `ReadModel` under `Source/`, `docs/adr/`, `docs/CONTEXT.md`: no hits). **Does not clear the bar.**

**(c) Split the header, same dispatch.** C++ needs the full class definition to call any member, and the inline per-frame getters dereference `GridLiveContext` (`focusedContext()`, `fieldCatalog()`). A panel that calls them needs the whole class. So a split can only trim includes the class definition does not need. Upper bound: dropping `AppController.h` would remove 38–55 files (median 42) from a Ui TU's include closure (re-measured with the closure script above over the 32 Ui includers; the count depends on how includes are resolved, so treat it as approximate). The floor a definition-preserving header keeps is at least `GridLiveContext.h` plus `ITrackerBackend.h` (7 files in their two closures together, themselves included). The real saving lies between and is not measured; finding it means auditing each of the 60 includes against the class body. Rebuild time is **not measured** here. Benefit is compile-time only; the fan-in count would not change unless the new header is counted separately, which the ratchet would then need to learn. **Unproven; not a reason to reopen on its own.**

**(d) Migrate only the 14 Ui includers that call no per-frame getter.** No named-getter Pillar-1 risk (text match only; `SmatchetActiveProjectGridCells.cpp` is a carve-out of the per-frame grid TU, so confirm it per file). But the record already classified this group as chain-coupled: draw signatures take `AppController&` (for example `SmatchetUserInfoUi::DrawWindow`, `DrawAssistantPreferencesTab`, `SmatchetDrawMcpServerPanel` in `Ui/SmatchetMcpServerUi.h`), and two carry `SMATCHET_DEVIATION(rule=app-controller-fan-in; …)` markers (`SmatchetActiveProjectGridCells.cpp`, `SmatchetBulkFieldEditPopup.cpp`). Each file needs new facet methods (for example the MCP activity calls, `OpenUrl`, `BuildIssueBrowseUrl`, `SetFocusedPane`) and its callers re-signed. Benefit: up to −14 fan-in, likely fewer per the T3 experience. Cost per file not measured. This is the T3 tier the record left as "chain-coupled-deferred", not a new finding.

### Recommendation

**Do not reopen. Option (a).** The measurements match the record: 32 Ui includers, the same per-frame getters, no read-model for product reasons, and the cheapest candidates still need surface no facet has. The earlier recommendation in this session was made without the record and should be withdrawn. If the owner wants fan-in work anyway, (d) is the only option with no named-getter Pillar-1 exposure (by the text proxy), and it belongs in the existing fan-in plan family as a T3 follow-up, not as a reopen of T6.

## Interface contracts

N/A — the recommendation is "do not reopen"; no interface changes.

## Files to modify

N/A — the recommendation is "do not reopen"; no files change.

## Existing utilities reused

- `appcontroller_fan_in_audit.py --list` / `--diff` — [`agent-layer/agents/scripts/core/appcontroller_fan_in_audit.py`](../../../agent-layer/agents/scripts/core/appcontroller_fan_in_audit.py) — the includer count and the ratchet.
- `AppController::GetAvailableFields` — [`AppController.h:840`](../../../Source/Core/include/AppController.h) — already a `shared_ptr`-to-const snapshot; the shape option (b) would reuse.
- `IAppSync`, `IAppFields` — [`Interfaces/IAppSync.h`](../../../Source/Core/include/Interfaces/IAppSync.h), [`Interfaces/IAppFields.h`](../../../Source/Core/include/Interfaces/IAppFields.h) — their header comments state the concrete-binding trade-off used above.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — the recommendation extracts nothing. Option (c) would split a header; it is not recommended and not sized.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact under (a). (b) adds per-frame snapshot cost; (c) none at runtime; (d) none if no per-frame getter moves.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact; no code changes.
- **Pillar 3 (never crash)**: no impact; no code changes.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no impact; no UI changes.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A — the recommended option changes no code under `Source/Core/`. If the owner picks (d) instead, each slice must name its `perf-pr-fast-set.json` scenario (gate 1); gates 2–5 would be N/A because no I/O, dispatcher, stall path or marker changes.

## Risks / non-goals

- **What would make this proposal wrong**:
  - A Ui read-model lands for product reasons (the record's own revisit trigger). Then (b) costs much less and should be re-costed.
  - A measured rebuild-time problem traced to `AppController.h` in Ui TUs. Then (c) has a benefit worth auditing.
  - The text proxy is wrong: some of the 18 do not reach a per-frame getter on a draw path, or some of the 14 do. A per-file audit would settle it.
  - CI perf evidence that one vtable lookup per frame on the inline getters is unmeasurable. The record treats that cost as forbidden on principle; data could justify relaxing it, but only the owner can relax a Pillar-1 rule.
- **Risk: the earlier session recommendation is acted on without this plan.** Mitigation: this plan records the withdrawal.
- **Risk: the documented baseline (70) disagrees with the live count (71).** Accepted: the audit treats the constant as informational; the merge-base ratchet is the enforcement and passes.
- **Risk: a stale reference.** `SmatchetBulkFieldEditPopup.cpp`'s deviation says "a narrow IAppGrid facet per ADR-0020"; ADR-0020 is `0020-sync-cache-seam-include-not-link-purity.md` and no `IAppGrid` exists. Out of scope here; flagged below.
- Save formats, backend leakage, Unreal vs standalone divergence, localization keys, MCP wire format, dual-target compile: no impact; no code changes.
- **Non-goal**: virtualising any per-frame getter. Ruled out by the record and not proposed.

## Implementation handoff

N/A — single decision, owner. No slices.

## Verification

- **Bucket A (pure-logic doctest, `test-authoring` skill § Unit rig)**: N/A — no code.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: N/A — no code.
- **Bash-driver scenario / screenshot / sanitizer**: N/A — no code. The figures above are reproducible with `python3 agent-layer/agents/scripts/core/appcontroller_fan_in_audit.py --list` and `--diff origin/develop`.
- **Build gate**: N/A — no code. Would be `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` for any accepted option.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required. Not run while staging this draft.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome. Required for every plan — do not delete. **Still owed**: this draft skipped it; the plan is not final until it runs.
- **Manual residue**: none.

## Open questions

- Is the 18 / 14 text-proxy split accurate per file? Answered by a per-file audit (probe), only if the owner picks (d).
- Should `BASELINE_FAN_IN` and the AGENTS.md row move from 70 to 71? Owner of the agent layer; informational only.
- **Decision request for the owner**: accept the recommendation (do not reopen; option (a)), pick another listed option ((b), (c) or (d)), or reject this plan.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- The stale `IAppGrid` / ADR-0020 reference in `SmatchetBulkFieldEditPopup.cpp`'s deviation — small follow-up fix, no plan needed.
- Option (d) as a T3 follow-up — its own plan in the fan-in family if the owner wants it.
- A rebuild-time measurement of `AppController.h` — needed before (c) can be argued.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.** Writing the actual basename here
   > manufactures a `docs/plans/shipped/<name>.md` path that points at a file
   > still living in `active/` (the move hasn't happened yet), which
   > `test-plan-ref-integrity.sh` reports as a dangling self-reference. The gate
   > carves out the *placeholder* form on the Archive `git mv` line; the
   > expanded form defeats that carve-out. Run the literal command with your
   > slug substituted at the shell — never bake the expansion into the file.
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier; PR #890), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*

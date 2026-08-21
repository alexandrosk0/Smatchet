# Smatchet — Code Review Backlog (2026-05-16 full rewrite)

> **Deprecated as a work queue (2026-07-06).** This is a closed historical ledger (last reconciled 2026-07-05) — do not file or hunt new work here. New agent-facing items go to the live self-improvement backlog ([`docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`](../docs/self-improvement/AGENT_SELF_IMPROVEMENT.md)); product bugs become GitHub Issues (ADR-0014).
>
> Scope: first-party C++ in `Source/Core/`, `Source/Plugins/`, `Source/Standalone/`.
> Method: skeleton + targeted reads + symbol grep against develop tip `7597fd7+` (post PR #39).
> Previous doc (2026-05-10..11) accumulated 62 numbered items, 87% of which landed. This rewrite drops all `✅ DONE` items and re-audits the remainder against current code.
>
> Status legend: P0 = bug / safety / build-break · P1 = significant code-health win · P2 = nice-to-have · P3 = note.
>
> Status markers: ✅ **RESOLVED** · 🟡 **PARTIAL** · ⏳ **OPEN** · 🚫 **CLOSED BY DECISION** — terminal. A deliberate won't-do-as-written or design-deferral: the question was answered and the answer was "not this". Not pending work, and not counted as open.
>
> Companions:
> - [`POST_P0_REVIEW.md`](POST_P0_REVIEW.md) — issues introduced by the P0 sweep. All 34 entries closed; its last partial (item 24, `FlushFileSink` shutdown wiring) was flipped ✅ on 2026-08-18 — see **A4** below.
> - [`MANUAL_TEST_QUEUE.md`](MANUAL_TEST_QUEUE.md) — deferred smoke tests.
> - [`RICH_TEXT_EDITING_V2_REMAINING.md`](RICH_TEXT_EDITING_V2_REMAINING.md), [`RICH_TEXT_EDITING_V3_PLAN.md`](RICH_TEXT_EDITING_V3_PLAN.md) — Markdown / ADF backlog.

---

> ## Triage — 2026-08-18 (post-merge, against develop tip `32feeaff`)
>
> One row closed, and one correction to this file's own earlier triage.
>
> - **C5 — 🟡 PARTIAL → ✅ RESOLVED (PR #2122).** `FileIo.{h,cpp}` now exists; `ScopedFileLock` has a
>   single definition at `Source/Core/include/FileIo.h:51` and `BackendAuditTrail` consumes it. See the
>   C5 section for why two of the row's three instructions were rejected rather than executed.
> - **Correction to the 2026-08-16 entry above.** That pass recorded C5 as *"closed as won't-do-as-written
>   (objective met in substance)"*. That judgement was wrong: the src-private header wall meant the lock
>   genuinely could not be shared, so the objective was NOT met in substance. The extraction was real work
>   and has now been done. The category-C count moves 6 ✅ → 7 ✅.
> - **DR29** (in `DEEP_REVIEW_2026-07-07.md`) also closed on 2026-08-18 via PR #2121 — recorded there, not
>   duplicated here.
>
> Nothing else in this file changed state. B1/B5 keep their 🚫 CLOSED BY DECISION markers from 2026-08-18;
> the `<details>` block below is a 2026-05-10 historical snapshot and is deliberately left untouched.

## Status snapshot (current — 2026-08-16)

| Category | Count | State |
|----------|-------|-------|
| A. P0 / safety tails | 4 | 4 ✅ (A1, A2, A3, A4) |
| B. P1 structural | 5 | 3 ✅ (B2, B3, B4) · 2 🚫 closed by decision (B1 won't-do-as-written · B5 improved, remainder design-deferred) |
| C. P2 polish | 7 | 7 ✅ (C5 resolved 2026-08-18 by PR #2122 — the 2026-08-16 entry called it won't-do; the extraction was then actually done) |
| N. New findings (2026-05-16 pass) | 14 | 13 ✅ · N13 ⏳ dormant watch (acceptable; no trigger fired) |

**Net open: zero work items.** N13 is a watch with an unfired trigger, not a task. The only actionable
residuals are outside this table: the A3 inline-grid-editor glyph (P3, fold-in) and archiving
`docs/plans/active/n4-trackeractions-interface.md` to `shipped/` (bookkeeping).

<details>
<summary>Historical snapshot (2026-05-10 baseline, superseded)</summary>

| Category | Count | State |
|----------|-------|-------|
| P0 originals | 10 | 9 ✅ · 1 🟡 (#9 BlameAnalysis split now ✅ — see N2 below) |
| P1 originals (11–31) | 21 | 19 ✅ · 2 🟡 (item 14 LuaAutomationHost · item 15 TrackerError) |
| P2 originals (32–61) | 30 | 23 ✅ · 7 ⏳ |
| New (62+) | 1 | ✅ DONE (priority hot-path PR #40) |
| Post-P0 review | 34 | 33 ✅ · 1 🟡 (B4 FlushFileSink shutdown) |

**Net open before this rewrite:** 14 carry-overs + new findings from this pass.

</details>

---

## Reconciliation pass — 2026-07-05 (develop tip ~PR #1620)

> This doc was written on 2026-05-16; the tree has since advanced ~1500 merged PRs, with major decomposition of the god-objects it tracks. Each still-open/partial item below was re-checked against current source. Item headings now carry a status marker (✅ RESOLVED · 🟡 PARTIAL · ⏳ OPEN · 🚫 CLOSED BY DECISION, added 2026-08-18). Note the file relocations: `ConfigManager.cpp`→`Config/`, `BuiltinCommands.cpp`→`Commands/Builtin/`, `PlaneClient.cpp`→`Tracker/Plane*.cpp`, the field-edit pipeline→`FieldEditPipelineService.cpp`.

| Item | Verdict | Current state |
|---|---|---|
| A1 TrackerConfig caching | ✅ RESOLVED | `ConfigManager::Load()` returns `GetCachedConfigRef()` under `GetCacheMutexRef()` (valid-flag cache, invalidated on `Save()`) — kills the repeated disk read/parse. |
| A2 Logger file-sink wiring | ✅ RESOLVED (differently) | Wired at startup in `Source/Standalone/main.cpp` + `android_main.cpp` via `Logger::SetFileSinkPath` (env `SMATCHET_DEBUG_LOG` / default path), **not** from `ConfigManager`; no `LogFilePath` key. Functional complaint closed. |
| A4 FlushFileSink on shutdown | ✅ RESOLVED (graceful) | `~AppController` now calls `Logger::Instance().FlushFileSink()` after `JoinBackgroundTasks()`, persisting the whole shutdown-sequence log trail before late teardown. The crash-handler half is intentionally **not** done — `SmatchetCrashHandler` is async-signal-safe and must not take the file-sink mutex mid-crash (it uses its own async-safe crash sink). |
| B1 LuaAutomationHost extraction | 🚫 CLOSED BY DECISION (2026-07-05 reframe; marker corrected 2026-08-18 — won't-do-as-written) | Ownership migration abandoned for a different design: `LuaAutomationHost` is now a 17-LOC log-sink coordinator; sol2 moved to a pImpl (see N10); the binding TU split 3 ways; `friend class LuaAutomationHost` removed; new `ILuaBindingHost` interface. |
| B2 TrackerHttpClient migration | ✅ OBJECTIVE MET (2026-07-05) | Uniform retry on transient failures is achieved — implemented at the `TrackerXxxLogged` helper layer, so every tracker HTTP path inherits it. 2B (`JiraClient::ProbeReachability`) migrated + shared `ClassifyReachabilityProbe`; 2C/2D mutations settled single-attempt (retry owned by the offline-queue replay loop); 2E (Jira search reads) verified already-retried via `TrackerGetLogged`. Repo sweep found one raw `cpr` verb (Jira multipart attachment upload) — now also applies `MakeTrackerSslOptions()` (Android CA-bundle parity, WS2/#1068). The consumer-side `IsTrackerTransportErrorText` retirement (N12) — the last named residual — shipped 2026-07-11; the function is deleted (tombstones at `Source/Core/src/Tracker/TrackerHttpPure.cpp:169` and `Source/Core/src/Tracker/TrackerHttpUtils.cpp:257`). Characterization guard: `tests/Core/JiraClientHttp.test.cpp`. |
| B3 ITrackerClient split | ✅ RESOLVED (exceeded) | `ITrackerClient` gone; replaced by `ITrackerBackend` composing 6 role interfaces (`ITrackerIssueReader`/`Connectivity`/`FieldCatalog`/`IssueMutations`/`Collaboration`/`Activity`). "Unsupported default-impl" pattern removed. |
| B4 Plane FetchIssuesForKeys | ✅ RESOLVED (B4-v2 landed) | Early-exit pagination in `Tracker/PlaneIssueSearch.cpp` stops once all keys matched. **B4-v2**: `FetchIssuesForKeys` now emits a server-side `sequence_id__in=<csv>` query param (via the pure, doctested `BuildPlaneSequenceIdInFilter`) when every requested key parses to a Plane sequence_id — O(1-2 pages) instead of scanning until the keys appear. Any unparseable key (bare UUID / malformed) abandons the filter so the server never excludes a still-needed issue; the early-exit sweep stays as the safety net. |
| B5 Markdown table-cell flatten | 🚫 CLOSED BY DECISION (improved 2026-07-05; remainder design-deferred — marker corrected 2026-08-18) | `MarkdownCellPlainInner` now joins cell blocks with `<br>` (GFM in-cell line break) and preserves list items instead of running paragraphs together / dropping lists. First ADF→Markdown table golden tests added. Deeper fidelity (code blocks, nested lists/tables in a cell) still deferred to RICH_TEXT_EDITING_V2. |
| C1 Retry-after-400 dup | ✅ RESOLVED | Consolidated into `FieldEditPipelineService::ApplyFieldUpdateWithEditMetaRetry`; both submit paths route through it. |
| C4 Plane customs dropped | ✅ RESOLVED (2026-07-10) | `BuildCreatePayload` now emits custom catalog fields under `properties.<uuid>` via the pure, doctested `BuildPlaneCustomProperties`; an unrepresentable value surfaces as `InvalidRequest` instead of silent loss. Edit-meta reporting of customs stays a flagged follow-on (seam has no catalog param). |
| C5 FileIo extraction | ✅ RESOLVED-IN-SUBSTANCE (2026-07-13) / won't-do-as-written | The sharing objective is **met**: `AtomicWriteTextFile` is a public `ConfigManager` static shared by all 5 atomic-write callers (Config `_Panes`/`_Views`/`_PathUtils`, `Tracker/FieldCatalogCache`, `Ui/SmatchetUI`); a repo sweep found **no remaining raw temp+rename re-implementations** to consolidate. The literal `FileIo.{h,cpp}` module move is now cosmetic-only: `ScopedFileLock` has **zero non-Config consumers** (so nothing outside `Config/` needs it hoisted), and `BackendAuditTrail` **appends** (`std::ios::app`) — a fundamentally different op that atomic-write-replace can't serve. Moving code to a new module home with no consumer to justify it = churn + gate risk (include/fan-in) for no functional win. Closed as won't-do-as-written; re-open only if a genuine third-party (non-Config, non-append) atomic-write consumer appears. |
| C6 LooksSensitiveKey blocklist | ✅ RESOLVED (documented, 2026-07-10) | The doc-why-each-entry-stays branch of the product call taken: rationale comment above `LooksSensitiveKey` classifies the list (credentials / identity-PII / free-text-that-quotes-both) and states why trimming is a privacy-stance decision. No behaviour change; trimming stays available if diff utility is later preferred. |
| N1 LuaBindings LOC | ✅ number stale | Now **1540** (not 2648) — the file shrank via the 3-way split, opposite the doc's "grew" narrative. |
| N3 CommandRegistry::FindLocked | ✅ RESOLVED | New alias-aware `Contains()` (locks internally, mirrors `FindLocked(name) != nullptr`); both `McpPlugin.cpp` worker-thread callers migrated to it — no registry pointer escapes to an httplib thread anymore. Regression-tested (alias resolution pinned). |
| N4 AppController.h size/friends | ✅ SUBSTANTIALLY RESOLVED (2026-07-13) | Part A (DTO extraction) done via the fan-in phases; Part B (`TrackerActions` interface) closed as MOOT per `docs/plans/n4-trackeractions-interface.md` — three friends collapsed to one `GridContextDepsAdapter` (the six-`I*Deps` adapter); sol2 friend gone. See §N4 for detail. |
| N5 ConfigManager.cpp size | ✅ RESOLVED / moot (2026-07-13) | `Config/ConfigManager.cpp` is now **463 LOC** (further split since the 1617 reconcile — logic lives across `_PathUtils`/`_Views`/`_Panes`/`_Internal.h`). Far under the 2000-LOC split trigger this watch named; the file SHRANK, so the watch is satisfied — no action. |
| N6 BuiltinCommands split | ✅ RESOLVED | Now a 72-LOC dispatcher + ~20 category files under `Commands/Builtin/`, exactly as proposed. |
| N8 OfflineQueueService friend | ✅ RESOLVED | Decoupled via `GridContextDepsAdapter` (see `AppController.h` comment). |
| N9 McpPlugin tools/list divergence | ✅ RESOLVED | Both REST + JSON-RPC paths registry-driven from `Commands().All()` + shared `BuildRunLuaToolEntry()`; can't diverge data-wise (cosmetic lambda dup remains). |
| N10 sol/sol.hpp public | ✅ RESOLVED | `AppController.h` no longer includes `<sol/sol.hpp>` — sol2 storage moved to a pImpl; only forward decls remain. |
| N12 IsTrackerTransportErrorText | ✅ RESOLVED (2026-07-11) | Transport-ness travels structurally end-to-end and the text heuristic is deleted. Slices: 1 (flag from composition seams), 12 (four backends classify `TrackerError` kinds), 13a (TrackerError-holding consumers), 13b (string-seam consumers carry `ErrorTransient`), 3 (Plane `ResolvePlaneProject` + streamed-exception classification, Jira mutation `Via*`/`AddIssueToSprint` re-threading, both seam fallbacks drop the sniff, `IsTrackerTransportErrorText` removed from `TrackerHttpPure.{h,cpp}`). Plan archived: `docs/plans/shipped/retire-transport-error-text.md`. |
| N13 TryGetMcpStatusSnapshot | ⏳ OPEN (acceptable) | Still a gated virtual on `IPlugin`; no capability-tag system. As the doc itself said, acceptable today. |

**Still genuinely open after this pass:** none blocking — N13 acceptable; B5/C5/N4 partial by design; B1 reframed (won't-do-as-written). N12 closed 2026-07-11 (heuristic deleted, plan archived to `docs/plans/shipped/retire-transport-error-text.md`); B2 objective met (uniform retry on transient failures; N12 was the last B2-adjacent piece). C4 (`properties.<uuid>` serialization) and C6 (blocklist rationale documented) closed 2026-07-10. A4 (graceful half) and N3 were closed 2026-07-05; B5 improved (multi-paragraph + list preservation) with fuller rich-cell fidelity deferred. Everything else on the A/B/C/N carry-over list is resolved. Items already ✅ in the doc (N2, C2, C7, N7, N11, N14) re-verified still true.

**Triple-check pass — 2026-07-13 (against develop tip):** re-verified every still-listed item against live code, not the reconcile text. Corrections: **N5** is now fully moot (`Config/ConfigManager.cpp` = **463 LOC**, split further; the watch is satisfied). **C5** is closed as won't-do-as-written / resolved-in-substance — the atomic-write sharing objective is met (5 callers via the `ConfigManager` static; **no raw temp+rename re-impls remain** to consolidate), `ScopedFileLock` has **zero non-`Config/` consumers**, and `BackendAuditTrail` *appends* (atomic-replace doesn't apply) — a `FileIo.{h,cpp}` module move is churn with no consumer to justify it. **B5** improved-half done (cell logic now in `Ui/AdfToMarkdown.cpp`, `<br>`-join + list items + golden tests); deeper block-content fidelity is design-deferred (GFM can't hold block content in a cell). **B1** confirmed done-differently (`LuaAutomationHost` = 17-LOC coordinator + `ILuaBindingHost`). **N13** still one gated virtual, no new plugin-type accessors landed → capability-tag system still not justified (acceptable). **Genuinely-open, being worked 2026-07-13:** **B4-v2** (server-side Plane `sequence_id__in` — verified NOT implemented, only a comment) + **C3** (edit-meta customs-reporting half — code comment confirms "customs stay unreported until the seam grows a catalog param"); and **N4** (AppController.h grew to **1512 LOC**, no `TrackerActions` — the one real remaining structural refactor; DTO-header slice + a staged interface plan in progress). ~~C3~~ **[superseded 2026-08-16: C3's customs-reporting half is implemented — `FetchIssueEditMeta` resolves the project and enumerates `FetchPlaneCustomFields`. See §C3 and the 2026-08-16 triage table.]**

**Reconcile — 2026-07-14:** two of the triple-check's "being worked" items have since settled. **B4-v2 has landed** — `smatchet::plane::BuildPlaneSequenceIdInFilter` (`Source/Core/src/Tracker/PlaneIssueMappingPure.cpp`, doctested in `tests/Core/PlaneIssueMappingPure.test.cpp`) is wired through the `FetchIssuesStreamed` URL builder into the page loop (`Source/Core/src/Tracker/PlaneIssueSearch.cpp`), matching §B4's description; the "verified NOT implemented" line above was true at check time and is superseded. **N4's staged interface plan has rendered its verdict**: `docs/plans/n4-trackeractions-interface.md` concludes **do not build `TrackerActions`; close N4 as substantially-resolved** (Part A DTO moves shipped via the fan-in phases; Part B moot) — pending that plan's final grill + archival. ~~**C3** (customs-reporting seam catalog param) remains open.~~ **[superseded 2026-08-16: verified implemented — see §C3 and the triage table. The plan's verdict has since been rendered; only its `active/` → `shipped/` move is outstanding.]**

**Triage — 2026-08-16 (against develop tip `7da969b`).** Re-checked every item this doc still leaves open, partial, or "pending", against live code rather than against the reconcile prose. Verdicts:

| Item | Prior state | Verified | Verdict / next action |
|---|---|---|---|
| **C3** Plane edit-meta customs | contradictory — §C3 says ✅ RESOLVED, the 2026-07-13/-14 reconciles say "remains open" | `PlaneFieldCatalog.cpp` `FetchIssueEditMeta` resolves the project from `cfg.JqlQuery`, calls `FetchPlaneCustomFields`, and marks each custom UUID editable (resolve/fetch failure non-fatal, built-ins still Ok) | **CLOSED.** §C3's ✅ is the accurate row; the "C3 remains open" lines in the 2026-07-13 and 2026-07-14 reconciles are **superseded**. Real per-issue *permissions* stay deferred (Plane v1 has no capability endpoint) — that is a Plane API limit, not a backlog item. |
| **A3** required-field UI | "shipped on branch `feat/required-field-ui-glyph` (PR pending)" | branch no longer exists (merged); red-`*` + `field.required_tooltip` glyph live in `Ui/SmatchetNewIssueDraftUi.cpp:585-600` and `TicketFieldEditor_Modal.cpp:193`; blank-required submit gated via the `missing` set | **CLOSED (shipped, not pending).** Residual, P3: the inline grid-cell editor (`TicketFieldEditor.cpp`) carries no required marker — a cell edit of a required field shows no affordance. Fold in when that file is next touched. |
| **N4** AppController.h / TrackerActions | "substantially resolved, pending plan grill + archival" | verdict rendered in `docs/plans/active/n4-trackeractions-interface.md` (do **not** build `TrackerActions`; Part A DTO moves shipped; fan-in 115 → 71) | **CLOSED as won't-do-as-written.** The only outstanding action is bookkeeping: move the plan `active/` → `shipped/`. No code work. |
| **N12** `IsTrackerTransportErrorText` | ✅ in the table, no marker in §N12 | function deleted; only tombstone comments remain (`TrackerHttpPure.cpp:169`, `TrackerHttpUtils.cpp:257`, two test notes) | **CLOSED.** §N12's missing status marker is a formatting miss, not an open item. |
| **N13** `TryGetMcpStatusSnapshot` | ⏳ OPEN (acceptable) | still exactly one `#if SMATCHET_WITH_MCP`-gated virtual on `IPlugin.h:39`; no second plugin-type accessor has landed | **STAYS OPEN / no action.** The trigger condition (a second accessor) has not fired in ~400 PRs. Re-check only when one does. |
| **B5** ADF→Markdown table cells | 🟡 IMPROVED (superseded 2026-08-18 → 🚫) | `MarkdownCellPlainInner` now lives in `Ui/AdfToMarkdown.cpp:252` with the `<br>`-join + list-marker behaviour | **🚫 CLOSED BY DECISION — design-deferred, not open work.** GFM cannot hold block content in a cell; the remainder is a representation decision owned by `docs/plans/active/rich-text-editing-v2-remaining.md`. |
| **B1** LuaAutomationHost, **C5** FileIo | reframed / won't-do-as-written | unchanged | **CLOSED.** No action; re-open criteria already recorded inline. |

**Triage — 2026-08-18 (marker reconciliation, against develop tip `5020f23`).** The 2026-08-16 pass rendered
correct verdicts but left the per-row markers rendering as open. Three rows re-verified against live code and
re-marked; no verdict changed, only the marker and the roll-up arithmetic.

| Item | Prior marker | Re-verified against code | New marker |
|---|---|---|---|
| **B2** N12 residual | ✅ OBJECTIVE MET, but §B2's heading read 🟡 IN PROGRESS and both the row and the § net named the `IsTrackerTransportErrorText` retirement as still-remaining | `IsTrackerTransportErrorText` has **no definition and no caller** in the tree. Tombstones only: `Source/Core/src/Tracker/TrackerHttpPure.cpp:169`, `Source/Core/src/Tracker/TrackerHttpUtils.cpp:257`, `tests/Core/TrackerHttpSslPure.test.cpp:134`, `tests/Core/OfflineQueueServiceRuntime.test.cpp:10`. Plan archived at `docs/plans/shipped/retire-transport-error-text.md` | ✅ **RESOLVED** — B2 has no residual |
| **B1** LuaAutomationHost | 🟡 REFRAMED (won't-do-as-written) | Reframe holds: `Source/Core/include/LuaAutomationHost.h:23` declares a log-sink-only class (`AddAutomationLogSink` / `ClearAutomationLogSinks` / `SnapshotLogSinks`), implemented in a 17-line `Source/Core/src/LuaAutomationHost.cpp`; `Source/Core/include/ILuaBindingHost.h` exists; no `friend class LuaAutomationHost` remains anywhere in `Source/`; sol2 state sits in the pImpl (`Source/Core/src/AppControllerImpl.h:171-174`). The literal 1B/1C/1D ownership migration is not going to happen | 🚫 **CLOSED BY DECISION** |
| **B5** ADF→Markdown table cells | 🟡 IMPROVED in three places (reconciliation row, 2026-08-16 triage row, § heading) despite the 2026-08-16 verdict of "design-deferred, not open work" | `MarkdownCellPlainInner` at `Source/Core/src/Ui/AdfToMarkdown.cpp:252` does the `<br>`-join and emits `- ` / `N. ` list markers; `:285` explicitly leaves codeBlock / nested table / mediaSingle unrepresented, with the rationale at `:245-251`. The remainder is a representation decision owned by `docs/plans/active/rich-text-editing-v2-remaining.md`, not queued effort | 🚫 **CLOSED BY DECISION** (all three mentions) |

> Roll-up corrected to match: category **B** now reads `3 ✅ · 2 🚫` (it previously claimed `4 ✅` while naming
> only three ✅ items — B1 and B5 were being double-counted as both ✅ and open). The new 🚫 marker is defined
> in the status-marker legend at the top of this file. **Untouched by this pass, deliberately:** **C5** and
> **DR29** (both under active work by others), and the 2026-08-16 row that pairs B1 with C5 — its verdict was
> already CLOSED and rewriting it would have disturbed C5.

**Net: this ledger has zero open work items.** N13 is a dormant watch; the only actionable residuals are the A3 inline-editor glyph (P3, fold-in) and archiving the N4 plan. Everything else is closed, deferred by design, or superseded.

---

## A. P0 / safety tails — finish these first

### A1. `TrackerConfig` caching — kill ~19 disk re-reads per UI action — ✅ RESOLVED (2026-07-05)
> `ConfigManager::Load()` now returns a cached `GetCachedConfigRef()` under `GetCacheMutexRef()` (valid-flag cache, invalidated on `Save()`); the repeated disk read/parse is gone. Cited line numbers below are stale (logic moved to `Config/ConfigManager.cpp` + `FieldEditPipelineService.cpp`).

Old item 3 tail. `ConfigManager::Load()` still hit on every field-edit submit and per-mutation:
- `Source/Core/src/AppController_CatalogAndFieldEdit.cpp` — **17 sites** (line 70, 137, 493, 598, 612, 871, 1009, 1075, 1094, 1112, 1126, 1135, 1150, 1159, 1176, 1185, 1205).
- `Source/Core/src/AppController_LuaBindings.cpp:676, 921`.
- `Source/Core/src/PlaneClient.cpp:1106, 1290`, `Source/Core/src/JiraIssueMutation.cpp:60, 481, 556`.

Each `Load()` opens + reads + parses ~50-field JSON. Plumb a `const TrackerConfig&` snapshot from the public entry of each pipeline; add a `ConfigManager::CachedSnapshot()` with a revision counter for read-only fast-path callers.

**Severity:** P1 (it's perf + lock churn, not correctness). Listed P0 because §6.3 of old doc tagged it that.

### A2. `Logger::SetFileSinkPath` never wired from `ConfigManager` — ✅ RESOLVED (2026-07-05, differently)
> The file sink is no longer dark: it's wired at startup in `Source/Standalone/main.cpp` + `android_main.cpp` (env `SMATCHET_DEBUG_LOG` / default path), not from `ConfigManager::Load()`, and there is no `LogFilePath` key. The functional gap is closed; the doc's specific prescription was not followed.

Old item 6 tail. Header API + worker thread exist (`Source/Core/include/Logger.h:72`, `Source/Core/src/Logger.cpp:197`); `ConfigManager.cpp` never calls it. File sink stays dark unless a test path is set manually. Wire on `ConfigManager::Load()` post-parse with the chosen log path; honour a new `LogFilePath` config key (or default `<userdata>/smatchet_runtime.log`).

### A3. `TrackerField::IsRequired` not consumed by UI — ✅ RESOLVED (verified merged 2026-08-16)
> Verified live: the red-`*` glyph + `field.required_tooltip` render in `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp:585-600` (required-ness fans in from the catalog's `IsRequired` **and** the issue type's `RequiredFieldIds`) and in `Source/Core/src/TicketFieldEditor_Modal.cpp:193`; blank-required submit is gated via the `missing` set. The `feat/required-field-ui-glyph` branch is gone — the "PR pending" note below is stale. **Residual (P3):** the inline grid-cell editor (`TicketFieldEditor.cpp`) has no required marker; fold in when that file is next touched.

Old item 10 tail. Schema populated for Plane (`PlaneClient.cpp` `TrackerFieldFromPlaneProperty`); zero UI consumer in `TicketFieldEditor.cpp` / `SmatchetNewIssueDraftUi.cpp`. Required-field state is dead data. Add a `*` glyph + tooltip on required-field labels in the new-issue draft and in-line editor; refuse submit / show validation on blank required. — shipped on branch `feat/required-field-ui-glyph` (PR pending).

### A4. `FlushFileSink` not called on shutdown / crash path — ✅ RESOLVED (graceful half, 2026-07-05)
POST_P0 item 24 (last open from that review). `Logger::FlushFileSink()` existed with no production caller. **Now wired** in `AppController::~AppController` immediately after `JoinBackgroundTasks()` — at that point every background thread that can emit a log line is joined, so the flush persists the entire shutdown-sequence trail to disk before member destruction and the riskier late-teardown steps run.

The original "call it from a `std::set_terminate` / `SIGSEGV` handler" ask is **intentionally not done** and is now superseded: this codebase grew a dedicated async-signal-safe crash handler (`Source/Standalone/SmatchetCrashHandler.cpp`) *after* this item was filed. That handler deliberately avoids the logger mid-crash — `FlushFileSink()` takes `m_fileSinkLifecycleMutex`/`m_fileSinkMutex` and waits on `m_fileSinkAckCv`, none of which is async-signal-safe (deadlock/UB risk in a signal context). Crash-time diagnostics are instead captured by the crash handler's own async-safe crash sink (marker + breadcrumb + minidump). So the abrupt-crash log batch is covered by a *different, safe* mechanism, and the graceful-shutdown drop-the-last-batch gap this item was really about is closed.

---

## B. P1 structural — open big wins

### B1. LuaAutomationHost Phase 1B → 1D (item 14) — 🚫 CLOSED BY DECISION (2026-07-05 reframe, won't-do-as-written; marker corrected 2026-08-18)
> The ownership migration below was abandoned for a different design: `LuaAutomationHost` is now a 17-LOC log-sink coordinator, sol2 moved to a pImpl (N10), the binding TU split 3 ways (`AppController_LuaBindings.cpp` **1540 LOC** + `_LuaBindingsCore.cpp` + `_LuaBindings_Draw.cpp`), the `friend` was removed, and a new `ILuaBindingHost` interface was introduced. The structural pain is addressed; the literal 1B/1C/1D extraction did not happen.

`Source/Core/src/AppController_LuaBindings.cpp` grew from 1453 → **2648 LOC** since the old review — Phase 1A migrated only log-sinks. Remaining:
- **1B** `InitLua` / `InitLuaCore` (~150–300 LOC). `<sol/sol.hpp>` stays PUBLIC on `AppController.h:11` until 1C; 1B alone is plumbing (low PR-value in isolation — see N1).
- **1C** ~18 `Lua*Bind` methods + free-function glue (~1500 LOC at current size). High-risk: patched-metatable contract per `CMakeLists.txt:355-411`.
- **1D** Automation worker thread (`AutomationWorkerLoop`, `automationJobMutex_`, shutdown contract).
- **2** Replace `friend class LuaAutomationHost;` with `TrackerActions` interface.

**Recommendation:** bundle 1B+1C as one PR with golden tests for each binding before move (testless extraction = lottery).

### B2. `TrackerHttpClient` migration follow-on (item 15) — ✅ RESOLVED (objective met 2026-07-05; last residual N12 closed 2026-07-11, re-verified 2026-08-18)
Phase 2A landed (PR #39) — helper + `PlaneClient::ProbeReachability`. ~30 hand-rolled error-status branches remain. Restarted "harness-first" 2026-07-05: `tests/Core/JiraClientHttp.test.cpp` now characterizes the real `JiraClient` HTTP-status→kind matrix over the `JiraCatalogHttpFixture` loopback (the harness already existed — used by `TrackerCatalogBuild`/`*Http` suites), so each migration batch has a regression guard. Migrate in batches:
- **2B** `JiraClient::ProbeReachability` — ✅ **done 2026-07-05** (routed through `ClassifyTrackerResponse`, mirrors Plane's Phase-2A switch; characterization test pins the status matrix). Reachability classification is now shared across both backends via `ClassifyReachabilityProbe` (extracted 2026-07-05 to kill the DRY clone flagged by the duplication lint; `PlaneClient`/`JiraClient`/`PlaneIssueSearch` all route through it).
- **2C** `PlaneClient::UpdateIssueFields` / `CreateIssue` / `FetchIssueEditMeta` — ✅ **decided 2026-07-05: mutations stay SINGLE-ATTEMPT, no per-call retry** (guardrail comments added at both call sites). Rationale:
  - `UpdateIssueFields` (PATCH) is driven by `OfflineQueueService::ReplayOneFieldEdit`, which already retries transient failures on its own tick with attempt bookkeeping. Wrapping the call in `TrackerHttpRequestWithRetry` would stack two retry loops and block the replay worker for the internal backoff. Direct (online) callers accept one attempt and surface a retryable `TrackerError`.
  - `CreateIssue` (POST) is **non-idempotent** — a retry after the server committed the create (5xx/timeout after receipt) would duplicate the issue. Durability for queued creates is owned by `OfflineQueueService::ReplayOneCreate` (pending-create latch de-dups).
  - `FetchIssueEditMeta` makes **no HTTP call** (`PlaneFieldCatalog.cpp` returns a static built-in field map), so there is nothing to retry. Nothing to migrate here.
  - Net: 2C is resolved by decision, not by new retry code. The single-attempt boundary is now documented in-code so a future contributor doesn't "helpfully" add a second retry layer.
- **2D** `JiraIssueMutation.cpp` mutation paths — same decision as 2C (mutations single-attempt; retry owned by the offline-queue replay loop). No code change; keeps Jira/Plane mutation semantics symmetric.
- **2E** `JiraIssueSearch.cpp` paginated fetches — ✅ **already retried 2026-07-05 (verified, no code change needed)**. The migration turned out to be implemented at the *helper* layer, not call-by-call: `TrackerGetLogged` (both overloads) already wraps `TrackerHttpRequestWithRetry` with the default idempotent predicate (Transport / 429 / 5xx), and every HTTP call in `JiraIssueSearch.cpp` (comment pages, JQL search pages, fetch-by-key, per-issue fallback, `myself` diagnose) routes through `TrackerGetLogged`. A repo-wide sweep for raw `cpr::Get/Post/Put/Patch` in `Source/Core/src/Tracker/` found **exactly one** verb that bypasses the helpers — the Jira multipart attachment upload (`JiraIssueMutation.cpp`), which can't use `TrackerPostLogged` (string-body only). That call already had the redirect guard + usage/log wiring; **2026-07-05 it also picked up `MakeTrackerSslOptions()`** (now exposed from `TrackerHttpUtils.h`) so it uses the same Android CA-bundle trust anchor (WS2 / Issue #1068) as every other tracker call instead of falling back to libcurl's default store. It stays single-attempt by design (non-idempotent POST).

**Net for B2: the migration objective — uniform retry on transient failures across every tracker HTTP path — is met.** Idempotent reads (GET) retry Transport/429/5xx via `TrackerGetLogged`; idempotent writes (PUT/PATCH) retry via `TrackerPutLogged`/`TrackerPatchLogged`; non-idempotent writes (POST + the multipart attachment) are Transport-only or single-attempt by design; and mutations additionally get durable retry from the offline-queue replay loop (2C/2D). The one remaining B2-adjacent item was the consumer-side **N12** cleanup — retiring the `IsTrackerTransportErrorText` string heuristic now that the tracker clients return structured `TrackerError` everywhere. **That shipped on 2026-07-11 and re-verified clean on 2026-08-18: the function has no definition and no caller anywhere in the tree.** What remains are tombstone comments only — `Source/Core/src/Tracker/TrackerHttpPure.cpp:169`, `Source/Core/src/Tracker/TrackerHttpUtils.cpp:257`, `tests/Core/TrackerHttpSslPure.test.cpp:134`, `tests/Core/OfflineQueueServiceRuntime.test.cpp:10` — plus historical prose in archived plans. **B2 has no residual; the row is ✅ RESOLVED.**

### B3. Split `ITrackerClient` into role interfaces (item 16) — ✅ RESOLVED (2026-07-05, exceeded)
> `ITrackerClient` no longer exists; replaced by `ITrackerBackend` composing 6 role interfaces (`ITrackerIssueReader`/`Connectivity`/`FieldCatalog`/`IssueMutations`/`Collaboration`/`Activity`). The "unsupported default-impl" pattern is gone (optional roles return `nullptr` accessors).

`ITrackerSearch` / `ITrackerMutation` / `ITrackerSchema` / `ITrackerUserDirectory` / `ITrackerWorkflow` (Jira-only). Removes the "unsupported default-impl" pattern (~7 virtuals). Stage with `dynamic_cast` at call sites. Mechanical PR.

### B4. `PlaneClient::FetchIssuesForKeys` O(N×total) (item 23) — ✅ RESOLVED (2026-07-05; B4-v2 server-side filter landed)
`PlaneIssueSearch.cpp` (file split from `PlaneClient.cpp`) still pulled every page then filtered in memory. Early-exit pagination stops fetching once every requested key has been matched — cuts the hot prefetch-open-links path from `O(total)` to `O(pages_until_keys_found)`. **B4-v2 (shipped)**: the `FetchIssuesStreamed` URL builder now threads an optional `sequence_id__in=<csv>` param through `FetchIssuesStreamedImpl → RunPlanePageLoop → FetchPlaneIssuePage`. `FetchIssuesForKeys` builds the CSV from the requested keys via the pure, doctested `smatchet::plane::BuildPlaneSequenceIdInFilter` (extracts the numeric suffix after the last `-` of each `PROJ-123` key; rejects bare UUIDs — whose final segment can itself be all-digits — and any malformed key, abandoning the whole filter so correctness never regresses). The server returns only the requested issues (O(1-2 pages)); the early-exit pagination remains as the fallback for the empty-filter case and any server that ignores the param. The public `FetchIssuesStreamed` full-sync override always passes an empty filter (a sync needs every issue).

### B5. Markdown table-cell rich content lost on ADF→Markdown (item 28) — 🚫 CLOSED BY DECISION (improved 2026-07-05; remainder design-deferred, marker corrected 2026-08-18)
> `MarkdownCellPlainInner` (now `Source/Core/src/Ui/AdfToMarkdown.cpp:252`, split out of `MarkdownConvert.cpp`) now collects each cell block's inline text and joins blocks with an HTML `<br>` (GFM's single-line-cell line break), and represents `bulletList`/`orderedList` items with markers — so multiple paragraphs and lists survive instead of being merged into one run or silently dropped. Added the first ADF→Markdown table golden tests (`tests/Core/MarkdownConvertAdf.test.cpp`). Remaining (deferred to RICH_TEXT_EDITING_V2, needs full round-trip golden coverage): code blocks, nested lists, and nested tables inside a cell — GFM can't hold true block content in a cell, so those need a design decision on representation — that decision, not effort, is what is outstanding, which is why this row is 🚫 rather than 🟡. The in-code comment at `Source/Core/src/Ui/AdfToMarkdown.cpp:245-251` and the `// Other block types (codeBlock, nested table / mediaSingle) remain unrepresented — see B5.` marker at `:285` record the boundary.
`MarkdownConvert.cpp` `MarkdownCellPlainInner` flattens. Tracked partly in `RICH_TEXT_EDITING_V2_REMAINING.md`; promote to its own ticket once round-trip golden tests land.

---

## C. P2 polish — leftovers

### C1. Two retry-after-HTTP-400 blocks (item 34) — ✅ RESOLVED (2026-07-05)
> Consolidated into `FieldEditPipelineService::ApplyFieldUpdateWithEditMetaRetry`; both submit paths route through it (`ErrorTextContainsHttpStatus` moved into the pipeline service).

`AppController_CatalogAndFieldEdit.cpp:837` and `:1009` both `if (!updateOk && ErrorTextContainsHttpStatus(outError, 400)) { ... refetch edit-meta ... retry }`. Extract `SubmitWithEditMetaRetry(issueId, field, payload, ...)` helper.

### C2. `MarkdownConvert::EmitInlineText` per-node vector allocs (item 38) — ✅ shipped (branch `feat/markdown-emitinlinetext-scratch`)
Rebuilds `openWrap` / `closeWrap` vectors per text node. Reuse a scratch buffer member. Done via `thread_local std::vector<const char*>` (capacity persists, mark markers are all string literals so no `std::string` heap churn).

### C3. `PlaneClient::FetchIssueEditMeta` hardcoded fields (item 39) — ✅ RESOLVED (customs-editable half; permissions stay deferred)
`PlaneFieldCatalog.cpp`. Reports 9 built-ins matching what `BuildCreatePayload` / `BuildUpdatePayload` / `AddIssueToSprint` serialize (`summary, description, priority, status, assignee, labels, sprint, type, parent`). **C3 (shipped)**: `FetchIssueEditMeta` now also enumerates the custom (work-item property) catalog fields and marks each UUID editable — it resolves the project from `cfg.JqlQuery` (same pattern as the mutation path) and calls the same `FetchPlaneCustomFields` the catalog build uses. This fixes the real defect C4 left: `EditMetaCacheService::CanEditFieldForIssue` treats a loaded edit-meta map's *unknown* keys as NON-editable, so before this change every custom field showed read-only in the field editor even though the create/update payloads accepted it. Resolve/fetch failure is non-fatal (built-ins still return Ok; customs simply stay unreported — the prior behaviour), so a config/permission hiccup never regresses built-in editability. **Still deferred**: real per-issue *permissions* — Plane v1 has no capability endpoint, so the server keeps the final say via the normal mutation error path.

### C4. `PlaneClient::BuildCreatePayload` / `BuildUpdatePayload` drop customs (item 40) — ✅ RESOLVED (2026-07-10)
> `BuildPlaneCustomProperties` (`Source/Core/src/Tracker/PlaneCustomPropertyPure.cpp`, pure + doctested) types every custom catalog field present in the draft per family and `BuildCreatePayload` emits the result under `properties.<uuid>`; an unrepresentable value aborts as `TrackerErrorInvalidRequest` instead of vanishing. See `docs/plans/shipped/plane-custom-properties.md`. Follow-ons flagged there: `FetchIssueEditMeta` still reports built-ins only, and the inline-`properties` create shape awaits a live-server smoke.

`PlaneClient.cpp:1459-1501` handles 6 core IDs (`summary`/`description`/`priority`/`status`/`type`/`parent`/`assignee`). Any `TrackerField.Id` that's a UUID (custom property) is silently dropped. Iterate `catalog` for custom props and emit under `properties.<uuid>` or whichever shape Plane v1 accepts.

### C5. Extract `ScopedFileLock` + `AtomicWriteTextFile` (item 41) — ✅ RESOLVED (2026-08-18, PR #2122)
> `Source/Core/{include,src}/FileIo.{h,cpp}` now exists under `smatchet::fileio` at include-DAG layer 0. `ScopedFileLock` has exactly one definition, `FileIo.h:51` (was src-private in `Config/ConfigManager_Internal.h`, whose own doc comment forbids inclusion from non-Config TUs — a hard wall that made the lock unusable outside `Config/`). `BackendAuditTrail` takes it at `:234` / `:251`.

**Resolved narrower than the row proposed, deliberately — two of its three instructions were wrong:**

- **`AtomicWriteTextFile` was NOT moved.** It is already a public `ConfigManager` static shared by 6 TUs; relocating it would touch 6 call sites to change nothing. The row's premise ("both still defined in `ConfigManager.cpp` anonymous namespace") was stale — the god-file had since been split into `Config/`.
- **"`BackendAuditTrail` uses raw `ofstream` → make it use `AtomicWriteTextFile`" is the wrong fix.** It *appends* (`ios::app`). Atomic whole-file replace on an append-only JSONL log means re-reading and rewriting the entire trail per event — O(n²) plus a fresh data-loss window on every write. What it actually lacked was the **lock**: `AuditMutex()` serialises one process, nothing enforces single-instance, and append is not cross-process atomic. It now takes the sidecar lock under the existing mutex.
- **`ReadRecentEvents` is deliberately left lock-free** — it is UI-thread reachable and already tolerates a torn tail, so a blocking `flock` there would be a Pillar-2 violation. Do not "finish" the row by adding one.

Residual risk stated honestly: the blocking-order tests are POSIX-only, so Windows CI proves acquisition/release but not contention, and no test on any platform exercises the true cross-*process* case the audit-trail change targets.

### C6. `BackendAuditTrail::LooksSensitiveKey` blocklist (item 43) — ✅ RESOLVED (documented, 2026-07-10)
> Took the "document why each entry stays" branch of the product call: a rationale comment above `LooksSensitiveKey` (`Source/Core/src/Persistence/BackendAuditTrail.cpp`) classifies the blocklist into credentials / identity-PII / free-text-that-quotes-both and records why the audit trail (plaintext, on-disk, travels in bug reports) keeps all three. Behaviour unchanged. If diff utility is ever preferred over the privacy stance, trimming is a deliberate follow-up decision, not a cleanup.

Redacts `summary` / `assignee` / `body` / `text` etc. Likely too broad — audit dumps lose useful diffs. Product call: trim the list or document why each entry stays.

### C7. Manual `PushClipRect` per grid cell (item 54) — ✅ shipped (branch `feat/grid-pushcliprect-audit`)
`Source/Core/src/SmatchetActiveProjectGridUi.cpp:843, 905, 930`. ImGui table already clips columns. Profile and remove if redundant. Removed; all three sites verified inside `BeginTable("TicketGrid")` scope.

---

## N. New findings (2026-05-16 pass)

### N1. `AppController_LuaBindings.cpp` at 2648 LOC — almost 2× since baseline (P1) — ✅ number stale (now **1540 LOC**, 2026-07-05)
> The file shrank via a 3-way split (`_LuaBindings.cpp` 1540 + `_LuaBindingsCore.cpp` 329 + `_LuaBindings_Draw.cpp` 1294) — the opposite of the "grew" narrative below. See B1.

Was 1453, now **2648**. Phase 1A of item 14 was supposed to start shrinking it; instead it grew. New surface includes `commands.invoke` glue, `RunAutoScript`, `RunFlatScriptAsync`, ~6 new `LuaUi*Bind`, window-op queue. The pre-existing extraction plan now has more to move; Phase 1C bundle is bigger than the doc predicted. Re-scope before committing.

### N2. `BlameAnalysisUi.cpp` split landed but not noted (✅)
Old item 9 step (c) was OPEN. Now done: `BlameAnalysisUi_Config.cpp` / `_Launch.cpp` / `_Modals.cpp` / `_Preferences.cpp` / `_Window.cpp` / `_Worker.cpp` + `BlameAnalysisUi_Internal.h`. Move from OPEN → done in tracker. Unreal hot-reload survival check still pending (no test infra).

### N3. `CommandRegistry::FindLocked` is a footgun (P2) — ✅ RESOLVED (2026-07-05)
> Closed via the second offered remedy (lock internally, return a value not a pointer). Added `CommandRegistry::Contains(name)` — alias-aware, takes the registry mutex internally, and returns a `bool` that exactly mirrors `FindLocked(name) != nullptr`. Both `McpPlugin.cpp` httplib-worker callers now use `Contains(name)`, so no `FindLocked` pointer escapes to a worker thread. `HasExact` was **not** the right substitute here: it is exact-only, so it would have broken the legacy-MCP aliases (`list_active_tickets` / `search_active_tickets`) by routing them to a fallback handler instead of the registry — a regression the new `Contains` test pins against. `FindLocked` is kept (renamed not needed) for the UI/Dispatch callers that legitimately dereference the command under the UI thread / internal lock.

`Source/Core/include/Commands/CommandRegistry.h:46` and `src/Commands/CommandRegistry.cpp:45-58`. Method name implies lock held but the impl is **lockless** — the comment says "Caller must serialize externally if they want stable pointers." External callers do NOT serialize: `Source/Plugins/Mcp/McpPlugin.cpp:629, :860` call it from httplib worker threads checking `!= nullptr` only. Race is benign today (pointer-equality test) but the API will bite future callers that dereference. Either:
- Rename to `FindUnlocked` + add `Has(name)` for the only existing real use, **or**
- Take the mutex internally and return a copy (`Optional<Command>`-style via `bool Find(name, Command& out)`).

### N4. `AppController.h` grew to 1035 LOC (was 729) (P1) — ✅ SUBSTANTIALLY RESOLVED (Part A done; TrackerActions moot, 2026-07-13)
> **Part A (DTO extraction) — DONE.** Every cross-concern DTO the refactor list below names is already out of `AppController.h`: `TrackerIssueFetchPack` + `TrackerConnectivityBannerForUi` → `Sync/SyncTypes.h`; `AppUpdateAsset`/`AppUpdateInfo` → `Types/AppUpdateTypes.h`; the six offline-queue `*Summary` structs → `Sync/OfflineQueueTypes.h` (re-exported via in-class `using`-aliases); `FieldEditResult`/`TrackerConnectivityState`/`AttachmentDescriptor` → `Types/` (shipped across fan-in Phases 1-6, post-dating the 2026-07-05 note). The only inline structs left are the private nested POD types `BackgroundWorker`/`AutomationJob` (AppController-owned, no fan-in benefit to moving). Fan-in is **71** includers (was baseline 115).
> **Part B (`TrackerActions` interface) — MOOT, do not build.** Assessment: `docs/plans/n4-trackeractions-interface.md`. The three service-friends the ledger flagged are already gone; the one remaining `friend GridContextDepsAdapter` is the *adapter that implements six `I*Deps` interfaces* — the typed action-contract `TrackerActions` was meant to provide already exists. Building `TrackerActions` would widen `AppController`'s virtual surface by ~10 members to delete one friend line on an owned 166-line translation class, deliver ZERO fan-in reduction, and not shrink the header. Re-open only if a NEW service needs a second friend — at which point the deps-interface pattern (not `TrackerActions`) is the answer. (Header LOC is method declarations + doc comments, not DTOs — the fan-in Phase 6 § T6 terminal-ceiling question, not a coupling question.)

Net +306 lines of public surface — `friend` declarations, forward decls, new structs (`CommandRegistry` accessors, `ScenarioRunner`, `MainThreadDispatcher`), `IsOnUiThread()`, etc. Old §1.3 P1 (cross-concern struct definitions forcing TU recompile) is now worse. With three services already extracted (`Ticket/Offline/Lua`), each service's own DTOs can move into its own header. Concrete refactor:
- `TrackerConnectivityBannerForUi` → already small, keep here.
- `TrackerIssueFetchPack` → move to `TicketSyncService.h`.
- `AppUpdateAsset` / `AppUpdateInfo` → new `AppUpdateClient.h`.
- All `*Summary` structs the §1.3 P1 callout listed → matching service header.
- Forward declarations of `OfflineQueueService` / `TicketSyncService` / `LuaAutomationHost` are fine; but `friend` of all three is a code-smell siren — every `private:` member is effectively public to ~70% of the codebase. The Phase 2 step (per old §1.7 design proposal #4: `TrackerActions` interface) is overdue.

### N5. `ConfigManager.cpp` grew to 1773 LOC (was 1333) (P2) — ✅ number stale (split into `Config/`, 2026-07-05)
> Split into a `Config/` directory: `ConfigManager.cpp` 1617 + `_PathUtils.cpp` + `_Views.cpp` + `_Panes.cpp` + `_Internal.h`. The per-concern split this item anticipated partly happened.

Post-split was supposed to be ~1333 LOC. Growth = +440 LOC. Spot-check: new config keys + bootstrap helpers. No structural issue, but the file is approaching the size threshold (~2000 LOC) where it should split per-concern (Tracker / Views / Config-file-IO / DPAPI). Track and split next time it hits 2000.

### N6. `BuiltinCommands.cpp` at 1898 LOC (P2) — ✅ RESOLVED (2026-07-05)
> Now a 72-LOC dispatcher + ~20 category files under `Commands/Builtin/` (`_View`/`_Perf`/`_Scenario`/`_Fields`/`_Tickets`/…), exactly the split proposed below.

New file — central registration of all CLI/Palette/MCP/Lua commands. Single `RegisterBuiltinCommands(reg, app)` function. At ~1900 LOC it's already a god-function risk. Split by category into `BuiltinCommands_View.cpp` / `_Perf.cpp` / `_Scenario.cpp` / `_Issue.cpp` / `_Field.cpp` etc. before it grows further; the existing `ViewCommands.cpp` precedent shows the pattern works.

### N7. `MarkdownConvert.cpp` at 1700 LOC, still no golden tests (P1) — ✅ DONE
~~Round-trip Markdown ↔ ADF / HTML continues to grow; bootstrap golden tests.~~ Closed since: `tests/Core/MarkdownConvert.test.cpp` covers the round-trip converters in `SmatchetTests`, and `tests/fuzz/fuzz_markdown_adf.cpp` fuzzes the ADF path. Flagged stale by `TEST_COVERAGE_GAP_MAP.md` § Hygiene notes.

### N8. `OfflineQueueService` still friend-coupled to AppController (P1) — ✅ RESOLVED (2026-07-05)
> `OfflineQueueService` + `TicketSyncService` friends were replaced by a single `GridContextDepsAdapter` friend during the item 11/12 Phase 2 extraction (see `AppController.h` comment).

`Source/Core/include/AppController.h:88-93` documents the friend-access boundary with a TODO to lift to interfaces. With Phase 1A→1C complete the service is at 1032 LOC of standalone logic plus AppController-private reach-throughs. Define the minimal access bundle (`IOfflineCacheAccess { Cache(), FindFieldById(), backendAuditTrail() }`) and convert. Same for `TicketSyncService` (currently friend-coupled).

### N9. `McpPlugin.cpp` REST `tools/list` + JSON-RPC `tools/list` may have diverged again (P2) — ✅ RESOLVED (2026-07-05)
> Both paths are registry-driven from `Commands().All()` + shared `BuildRunLuaToolEntry()` — no hardcoded duplicated payload; they can't diverge data-wise (only a cosmetic copy-pasted `std::transform` lambda remains).

Old §5.1 P1 was a duplicated payload (REST `:506` vs JSON-RPC `:666`). PR #41 / #52 added `BuildRunLuaToolEntry()` for the `run_lua` row. Re-check: are the rest of the tool-list rows also shared, or did the registry-driven approach drift? Audit current `McpPlugin.cpp:586` (REST) vs JSON-RPC handler.

### N10. `<sol/sol.hpp>` STILL PUBLIC on `AppController.h:11` (P1, item 14 dependency) — ✅ RESOLVED (2026-07-05)
> `AppController.h` no longer includes `<sol/sol.hpp>` — sol2 storage moved to a pImpl (`Impl`), so the ~100 header includers no longer drag sol2 through the compiler. Only forward decls remain.

Worth flagging on its own. Every TU including `AppController.h` (which is most of `Source/Core/`) drags ~1 MB of sol2 templates through the compiler. Phase 1C of item 14 is the only thing that unblocks this. The build-time win is real and measurable (`SmatchetPch.h` comments narrate sol2 as the heaviest header).

### N11. No `tests/` directory still exists (P0 for any future refactor) — ✅ DONE
~~Bootstrap a minimal doctest target before any further extraction.~~ Closed since: the doctest rig exists at scale (`tests/` holds 270+ test files across `SmatchetTests`, `SmatchetTsanTests`, UI, fuzz, Lua, and bats suites) and every candidate unit listed here (FuzzyMatch, MarkdownConvert, JqlProjectScope, TextMerge, CompactDateFormat) is covered. Flagged stale by `TEST_COVERAGE_GAP_MAP.md` § Hygiene notes; the remaining per-TU gaps are tracked there, not here.

### N12. `IsTrackerTransportErrorText` heuristic still classifies (P2) — ✅ RESOLVED (2026-07-11, re-verified 2026-08-16)
> The function is deleted; only tombstone comments remain (`Tracker/TrackerHttpPure.cpp:169`, `Tracker/TrackerHttpUtils.cpp:257`). Transport-ness travels structurally end-to-end — see the reconciliation table row for the slice breakdown. The description below is historical.

`Source/Core/src/TrackerHttpUtils.cpp:161-236` — string-pattern-matching error text. `TrackerError` now classifies properly via HTTP status. `IsTrackerTransportErrorText` should disappear once item 15 migration completes; until then it shadows the new mechanism and produces inconsistent classifications when callers mix the two.

### N13. `IPlugin::TryGetMcpStatusSnapshot` couples the host to MCP (P3)
PR #20 replaced `dynamic_cast<McpPlugin*>` with `virtual bool TryGetMcpStatusSnapshot(McpServerStatus&)`. Cleaner, but the virtual is `#if SMATCHET_WITH_MCP`-gated on the base class — every plugin now ships a conditional v-table slot. Acceptable today; if more plugin-type-specific accessors land, switch to capability tags / `IPluginCapability* GetCapability(CapabilityId)` so the base interface stays MCP-agnostic.

### N14. Annotate config hydrate does sync disk I/O on the UI thread (P2, Pillar 2) — ✅ DONE
~~`AnnotateAnalysisUi::ensureSettingsBuffersLoaded()` → `HydrateAnnotateCfgDiskOnce()` calls `ConfigManager::LoadAnnotateAnalysis` synchronously on the UI thread.~~ **Resolved** across PR #568 (`annotate-async-config-hydrate`) + PR #574 (`config-io-safe-coalesced-writes`):
- **Load**: measured at **0.54 ms** (small once-guarded whole-file JSON read) — kept synchronous with a `PILLAR2_INLINE` annotation per the documented inline-vs-async hydration policy (a background-thread + dispatcher round-trip isn't worth it for a sub-ms one-time read).
- **Save**: the larger Pillar-2 exposure (per-edit `SaveAnnotateAnalysis` from UI callbacks, surfaced by CodeRabbit on #565) is now off the UI thread via the single coalescing `smatchet::config_save` worker, with `GetConfigRmwMutexRef` serializing the read-modify-write so concurrent writers can't lose updates.

---

## Sequencing (current — 2026-08-16)

Every numbered step of the 2026-05-16 sequencing plan has landed or been closed by decision. What
remains is not a sequence:

1. **A3 residual (P3)** — add the required-field marker to the inline grid-cell editor
   (`TicketFieldEditor.cpp`). Fold in when that file is next touched; not worth its own PR.
2. **N4 bookkeeping** — move `docs/plans/active/n4-trackeractions-interface.md` to `shipped/`.
   The verdict is rendered; only the archive move is outstanding.
3. **N13 watch** — no action. Re-check only if a second plugin-type accessor lands on `IPlugin`,
   which would justify the capability-tag system this item describes.

Deferred by design, tracked elsewhere (do not sequence here):

- **B5** 🚫 deeper table-cell fidelity → `docs/plans/active/rich-text-editing-v2-remaining.md`.
  GFM cannot hold block content in a cell; this needs a representation decision, not effort.
- **B1** / **C5** — closed as won't-do-as-written, with re-open criteria recorded inline.

<details>
<summary>Original sequencing (2026-05-16, all steps closed — kept for provenance)</summary>

**Now (low-risk, high-leverage):**
1. **N11** — bootstrap doctest `tests/` with 3–4 starter units. Unblocks everything else.
2. **A1** — `TrackerConfig` cached snapshot + revision counter. Single-PR mechanical refactor; measurable UI-latency win.
3. **A2** — wire `Logger::SetFileSinkPath` from `ConfigManager`. ~20-line change.
4. **A4** — call `FlushFileSink` from `~AppController` + crash handler. ~5-line change.

**Next (medium PRs):**
5. **B3** — split `ITrackerClient` into role interfaces. Mechanical.
6. **B2** — ✅ objective met (uniform retry via the `TrackerXxxLogged` helper layer; 2B/2C/2D/2E all resolved). Only **N12** (retire `IsTrackerTransportErrorText`) remains as follow-on.
7. **N4** — move service DTOs into their own headers; start chipping at `AppController.h` size.
8. **N6** — split `BuiltinCommands.cpp` per category.

**Defer until tests land (N11):**
9. **B1** — LuaAutomationHost 1B + 1C as a single PR. ~1700 LOC moves; needs golden tests.
10. **N7** / **B5** — Markdown rich-content fixes. Needs round-trip golden tests.

**Standing:**
- C1–C7, A3, B4: small enough to fit when adjacent work touches the file.
- N5, N9, N12, N13: re-check next pass.

</details>

---

## Out of scope here

- Rich-text gaps → [`RICH_TEXT_EDITING_V2_REMAINING.md`](RICH_TEXT_EDITING_V2_REMAINING.md), [`RICH_TEXT_EDITING_V3_PLAN.md`](RICH_TEXT_EDITING_V3_PLAN.md).
- Deferred runtime smokes → [`MANUAL_TEST_QUEUE.md`](MANUAL_TEST_QUEUE.md).
- Cppcheck periodic-sweep runbook → [`CPPCHECK_PLAN.md`](CPPCHECK_PLAN.md).
- Post-P0 review trail → [`POST_P0_REVIEW.md`](POST_P0_REVIEW.md).

_End of backlog._

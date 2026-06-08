# Plan — De-duplicate tracker query-suggest + AI-client SSE clones

> **Slug**: `dedup-tracker-query-and-ai-client-clones` (matches this file's basename without `.md`).
>
> **Status**: `active` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section. Sections that genuinely don't apply get `N/A — <one-line reason>`, not deletion — the headings drive the "did you consider this?" forcing function for every author + reviewer agent.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The DRY duplication gate (`agents/scripts/core/dup_audit.py`, WARN-first per [process.md](../../self-improvement/categories/process.md) `dry-pillar-dup-gate` entry + [ADR-0015](../../adr/0015-dry-quality-pillar-duplication-gate.md)) reports **449 grandfathered cross-file clones** ≥ 70 normalized tokens. A manual read of the top offenders sorts them into four classes, two of which are genuine copy-paste that predates the gate and should be folded into shared helpers, one deliberate, and one false positive:

| Cluster | Tokens | Pair(s) | Class |
|---|---|---|---|
| **A** | 1288 / 466 / 199 / 192 | `JqlSuggestEngine.cpp` ↔ `PlaneQuerySuggestEngine.cpp` | TRUE clone → extract |
| **B** | 492 (+ Ollama) | `AnthropicClient.cpp` ↔ `OpenAiClient.cpp` ↔ `OllamaClient.cpp` | TRUE clone → extract |
| **C** | 415 / 377 | `AppController_LuaBindings.cpp` ↔ `AppController_LuaBindingsCore.cpp` | DELIBERATE (documented) → leaf-extract or exempt |
| **D** | 337 / 269 / … | `JiraClient.h` ↔ `PlaneClient.h` ↔ `GitHubClient.h` | FALSE POSITIVE → never extract |

**Intended outcome — after this lands:** the two real clone clusters (A, B) are single-sourced; cluster D no longer pollutes the calibration sample (it is skip-regioned or exempted, not refactored); and the work is recorded as the first real WARN→block calibration data point for the DRY pillar gate. Prompted by an interactive review of the gate output (2026-06-07 session); no incident.

## Approach

**Cluster A — extract backend-agnostic query-suggest helpers (primary win).** Both engines open with a ~140-line anonymous-namespace block of *identical* string/catalog utilities that differ only by cosmetic identifier renames (`IsJqlIdChar`↔`IsPlaneIdChar`, `JqlQuotedValue`↔`QuotedValue`, `FindTrackerFieldForJqlToken`↔`FindFieldToken`, `AddSuggestionUnique`↔`AddUnique`, `AppendJqlTerms`↔`AppendTerms`, `AppendFieldCatalogSuggestions`↔`AppendFieldCatalog`, plus byte-identical `ScanStringStateToCursor` / `AsciiEqualsIgnoreCaseToLowered` / `AsciiStartsWithIgnoreCase` / `IsUserField` / `IsDateField`). None contain JQL-vs-Plane grammar — they are pure quoting / prefix-match / field-catalog iteration. Move them verbatim into a new `Source/Core/src/Tracker/TrackerQuerySuggestCommon.{h,cpp}` with one canonical name each; both engines `#include` it. This is shared *implementation*, not a shared *interface*, so it does **not** violate the Tracker `AGENTS.md` "no backend leak into shared interfaces" invariant (it never touches `ITrackerBackend` or its role interfaces). The backend-specific grammar (operator/keyword tables, the `Suggest()` entry point, value-suggestion specifics) stays in each engine.

**Cluster B — extract the SSE drive skeleton behind a per-provider dispatch callback.** All three AI clients share a byte-identical streaming skeleton: `AiSseParser` setup → `cpr::WriteCallback` with the double cancel-check → `cpr::Post` → `NetworkUsageTracker::Record` → `Flush` → the cancel / transport-error / HTTP-status-redact / EOF-final blocks. The only per-provider variation is (1) the header map, (2) the event-translation function (`DispatchAnthropicEvent` vs `DispatchOpenAiDataLine` vs `DispatchOllamaLine`), (3) the log prefix, and (4) Anthropic's extra `pendingFinishReason` state. Introduce `Source/Core/src/AiSseStream.{h,cpp}` exposing one function — `StreamSseRequest(url, headers, body, cfg, translateFn, onDelta, onError, cancel, providerName)` — that owns the skeleton and takes `translateFn` (the per-provider event dispatch) as a `std::function`. Each client shrinks to: build headers → define its translate lambda → call `StreamSseRequest`. The per-provider dispatch functions stay backend-local; only the mechanical transport loop is shared.

**Clusters C + D — bound, do not over-reach.** C (`JsonToLuaImpl`/`LuaToJsonImpl`) is an *intentional* copy — `AppController_LuaBindingsCore.cpp:59` documents it ("own private copy … the no-ImGui isolation is the point"). The only DRY-respecting extraction is a dependency-free leaf `Source/Core/src/LuaJsonConvert.{h,cpp}` (sol2 + nlohmann only, zero ImGui / AppController includes) that both TUs include; if that leaf would drag any unwanted include it is **not** worth it and C is instead annotated `SMATCHET_DEVIATION(rule=duplication; reason=intentional no-ImGui isolation; …)`. C is a **stretch goal** — ship A+B first. D is the C++ override-signature symmetry that every concrete tracker client must repeat to satisfy the shared `ITracker*` role interfaces; coupling it via a shared base would reintroduce the banned monolithic `ITrackerClient` and couple independent backends (review **CRITICAL** per Tracker `AGENTS.md`). The action for D is a `dup_audit.py` enhancement: skip the class-declaration override region of backend `*Client.h` headers (or, minimally, `SMATCHET_DEVIATION`-exempt the three header spans) so the FP stops inflating the calibration sample. D ships **no product-code change**.

## Files to modify

Grepped before naming: `git ls-files | grep -iE 'QuerySuggestCommon|StreamSse|AiSseStream|LuaJsonConvert'` → **no collisions** (names free).

**Cluster A (Tracker strict zone):**
1. `Source/Core/src/Tracker/TrackerQuerySuggestCommon.h` (new) — declarations of the shared query-suggest helpers (`QueryValueNeedsQuotes`, `QueryQuotedValue`, `InsertForValueToken`, `ScanStringStateToCursor`, `FindTrackerFieldForToken`, `AddSuggestionUnique`, `AsciiStartsWithIgnoreCase`, `AppendTerms`, `AppendFieldCatalog`, `IsQueryUserField`, `IsQueryDateField`). Declares against existing `QuerySuggestion` / `TrackerField`.
2. `Source/Core/src/Tracker/TrackerQuerySuggestCommon.cpp` (new) — the bodies, moved verbatim from one engine (canonical copy).
3. [`Source/Core/src/Tracker/JqlSuggestEngine.cpp:13`](../../../Source/Core/src/Tracker/JqlSuggestEngine.cpp) — delete the local helper block, `#include "TrackerQuerySuggestCommon.h"`, re-point call sites.
4. [`Source/Core/src/Tracker/PlaneQuerySuggestEngine.cpp:13`](../../../Source/Core/src/Tracker/PlaneQuerySuggestEngine.cpp) — same.

**Cluster B (AI-source-gated zone):**
5. `Source/Core/src/AiSseStream.h` (new) — `StreamSseRequest(...)` signature + the `TranslateFn` typedef.
6. `Source/Core/src/AiSseStream.cpp` (new) — the shared cpr-drive + error skeleton.
7. [`Source/Core/src/AnthropicClient.cpp:156`](../../../Source/Core/src/AnthropicClient.cpp) — replace inline skeleton with a `StreamSseRequest` call; keep `DispatchAnthropicEvent` + `pendingFinishReason` local (pass via capture).
8. [`Source/Core/src/OpenAiClient.cpp:156`](../../../Source/Core/src/OpenAiClient.cpp) — same.
9. [`Source/Core/src/OllamaClient.cpp:125`](../../../Source/Core/src/OllamaClient.cpp) — same.
10. [`CMakeLists.txt:908`](../../../CMakeLists.txt) — **add `AiSseStream.cpp` to `_SMATCHET_AI_SOURCES`** (the explicit list that is pruned from the glob then re-added only under `SMATCHET_WITH_AI`; a new AI-only TU left to the bare glob breaks the AI-OFF build). `TrackerQuerySuggestCommon.cpp` needs **no** CMake edit — it is picked up by the `file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "Source/Core/src/*.cpp")` at [`CMakeLists.txt:801`](../../../CMakeLists.txt).

**Cluster D (tooling, no product code):**
11. [`agents/scripts/core/dup_audit.py`](../../../agents/scripts/core/dup_audit.py) — add a header-override-region skip (suppress clones whose both endpoints are inside a backend `*Client.h` public override block), OR file the three header spans as `SMATCHET_DEVIATION(rule=duplication)`. Pick one in implementation; record which in § Deviations.

**Cluster C (stretch — may defer):**
12. `Source/Core/src/LuaJsonConvert.{h,cpp}` (new, optional) + [`AppController_LuaBindings.cpp:85`](../../../Source/Core/src/AppController_LuaBindings.cpp) + [`AppController_LuaBindingsCore.cpp:64`](../../../Source/Core/src/AppController_LuaBindingsCore.cpp) — only if the leaf pulls zero unwanted includes; else exempt in place.

## Existing utilities reused

- `ToLowerAsciiCopy` — [`Source/Core/include/StringUtil.h`](../../../Source/Core/include/StringUtil.h) — already the shared lower-case helper both engines call; the extracted helpers keep calling it (do not re-roll).
- `AiSseParser` — [`Source/Core/include/AiSseParser.h`](../../../Source/Core/include/AiSseParser.h) — the shared SSE line/frame parser; `StreamSseRequest` wraps the cpr feed loop around it, it is not re-implemented.
- `smatchet::ai::pure::RedactProviderErrorBody` — already shared; the extracted error block calls it unchanged (no secret-leak regression).
- `NetworkUsageTracker::Instance().Record` — already the shared traffic accounting sink; called once inside `StreamSseRequest`.
- `QuerySuggestion` struct + `TrackerField` / `TrackerFieldFamily` — [`Source/Core/include/TrackerFieldSchema.h`](../../../Source/Core/include/TrackerFieldSchema.h) — the shared types the query helpers operate on; no new type introduced.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — behaviour-identical mechanical extraction; same call counts, same allocations, the moved code is byte-for-byte the same logic. JQL/Plane autocomplete and AI streaming run the identical instruction sequence post-refactor.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — no new sync I/O; the SSE drive already runs on the AI worker thread (off the UI thread), and query-suggest is pure in-memory string work.
- **Pillar 3 (never crash)**: neutral-to-positive — single-sourcing removes the drift risk of fixing a bug in one twin but not the other. The one hazard is collapsing a *real* (non-cosmetic) difference between near-twins; mitigated by diffing each helper pair before folding and keeping all per-provider / per-grammar variation backend-local (callback seam for B, no grammar in A's shared set). RAII / bounds unchanged.
- **Pillar 4 (accessibility)**: N/A — no UI surface, theme, or locale change.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

Per [`docs/plans/shipped/pillar-1-2-perf-review-system.md`](../shipped/pillar-1-2-perf-review-system.md). Diff touches `Source/Core/` → gates apply.

1. **PR-fast CI** — **fires (as a no-regression guard)**. Closest scenarios: `AiAssistantStreamingHappyPathScenario` / `AiAssistantStreamingTransportDownScenario` (cluster B path) and the JQL-autocomplete suggest path (cluster A) per `agents/core/perf-gatekeeper.md` § Curated diff → scenario map. Expect flat deltas (behaviour-identical); the gate's job here is to *prove* flatness.
2. **Pillar 2 static scanner** — **N/A** — no new sync-I/O reachable from `ImGui::*`; the cpr drive is worker-thread-only and unchanged, query-suggest is pure CPU.
3. **Dispatcher drain** — **N/A** — does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — **N/A** — adds no new sync-stall path > 100 ms; no new blocking call.
5. **Marker inventory** — **N/A** — adds no `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: run [`docs/guides/perf-workflow.md`](../../guides/perf-workflow.md) § Gate-check vs baseline (Step 7) against the AI-streaming scenarios before opening the PR; confirm no regression vs the approved baseline.

**Override**: `perf-out-of-band` PR label per `AGENTS.md` § Merge gates — not expected (no intentional regression).

## Risks / non-goals

**Risks:**
- **Near-twin ≠ identical (correctness).** A's helpers and B's skeleton are *near*-identical; a silent behavioural difference (e.g. Anthropic's `pendingFinishReason` threading, or a subtly different char-class in one `IsIdChar`) could be flattened by a careless fold. *Mitigation:* diff each helper pair / skeleton block line-by-line before extracting; keep every genuine divergence backend-local (callback for B, grammar tables for A). The cosmetic-only renames are already confirmed for the A helper set and the B skeleton in the originating session.
- **Strict Tracker lint zone (A).** `TrackerQuerySuggestCommon.{h,cpp}` lands in `Source/Core/src/Tracker/` — every new violation fails CI on a delta basis (narrowing, no-raw-new, func-size, func-branchy). *Mitigation:* moved code already passes today; keep functions intact (no merging that crosses the 120-line / 30-branch caps).
- **Dual-target source gating (B).** Forgetting `AiSseStream.cpp` in `_SMATCHET_AI_SOURCES` breaks the `SMATCHET_WITH_AI=OFF` build (the bare glob would still include it). *Mitigation:* file #10 above is explicit; anchor `cmake --build … --target SmatchetStandalone SmatchetCore_DX12` and an AI-OFF configure to it.
- **dup gate self-consistency (D).** A header-override skip-region in `dup_audit.py` must not over-suppress real header clones. *Mitigation:* scope the skip narrowly to public override-decl blocks of files matching `*Client.h`; cover with a `--selftest` / bats case.

**Non-goals:**
- No GitHub query-suggest unification (there is no `GitHubQuerySuggestEngine`).
- No unification of the per-provider event-dispatch logic (`DispatchAnthropicEvent` / `DispatchOpenAiDataLine` / `DispatchOllamaLine` stay separate — that is the legitimate backend variation).
- No coupling of the tracker backend clients via a shared base (cluster D) — explicitly forbidden.
- Does **not** graduate the DRY gate WARN→block — that is the separate, calibration-gated follow-up in `process.md` (`dry-pillar-dup-gate`); this plan only *feeds* its sample.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest, `test-rig`)**: add `tests/Core/TrackerQuerySuggestCommon.test.cpp` exercising the extracted pure helpers (quoting round-trip, `ScanStringStateToCursor` string-state, prefix-match case-folding, field-catalog suggestion uniqueness) — these are pure functions and belong in bucket A. No new pure surface for B (the skeleton is I/O-bound) beyond what `AiSseParser` tests already cover.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: existing `AiAssistantStreamingHappyPathScenario` + `AiAssistantStreamingTransportDownScenario` must stay green — they are the behaviour-equivalence proof for cluster B (cancel, transport-down, HTTP-error, EOF-final all routed through the new `StreamSseRequest`).
- **Bash-driver scenario / screenshot / sanitizer**: run the AI-streaming scenarios under the per-PR UBSan build (no new UB surface expected); no screenshot diff (no visual change).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) **plus** one `-DSMATCHET_WITH_AI=OFF` configure to prove `AiSseStream.cpp` gating is correct.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the Tracker + AI domain models + sharpen terms (e.g. "query-suggest helper" vs "JQL grammar", "SSE drive" vs "event dispatch") before finalising; record the outcome. Required for every plan — do not delete.
- **DRY gate confirmation**: after each cluster lands, `python agents/scripts/core/dup_audit.py --diff origin/develop` shows the corresponding clone tokens **gone** (and no new WARN introduced); record before/after clone counts as the calibration data point.
- **Manual residue**: none expected. If the cluster-C leaf decision ends up needing a human eyeball on include hygiene, file a `docs/self-improvement/categories/tooling.md` entry rather than leaving silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here (notably cluster C if deferred, and the cluster-D dup_audit skip-region), and revise or delete stale "deferred-as-current" refs.

- **Cluster C full extraction** — stretch goal; may ship as a follow-up or be exempted in place. One-line follow-up: a `LuaJsonConvert` leaf only if it pulls zero unwanted includes.
- **DRY WARN→block graduation** — separate calibration-gated follow-up (`process.md` `dry-pillar-dup-gate`); no-action here beyond feeding the sample.
- **The other ~445 baseline clones** — not triaged in this plan; this plan addresses only the top TRUE-clone clusters. No-action until the gate graduates or another review pass triages them.
- **Tracker backend base-class extraction (cluster D product code)** — explicitly never; would violate the no-backend-leak invariant.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/dedup-tracker-query-and-ai-client-clones.md docs/plans/shipped/dedup-tracker-query-and-ai-client-clones.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier; PR #890), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*

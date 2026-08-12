# Plan — De-duplicate tracker query-suggest + AI-client SSE clones

> **Slug**: `dedup-tracker-query-and-ai-client-clones` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — clusters A + D shipped; B + C deferred-with-rationale (see § Deviations + the `debt.md` follow-up). The machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section. Sections that genuinely don't apply get `N/A — <one-line reason>`, not deletion — the headings drive the "did you consider this?" forcing function for every author + reviewer agent.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The DRY duplication gate (`agents/scripts/core/dup_audit.py`, WARN-first per [process.md](../../self-improvement/categories/process.md) `dry-pillar-dup-gate` entry + [ADR-0015](../../adr/0015-dry-quality-pillar-duplication-gate.md)) reports **449 grandfathered cross-file clones** ≥ 70 normalized tokens. A manual read of the top offenders sorts them into four classes, two of which are genuine copy-paste that predates the gate and should be folded into shared helpers, one deliberate, and one false positive:

| Cluster | Tokens | Pair(s) | Class |
|---|---|---|---|
| **A** | 1288 / 466 / 199 / 192 | `JqlSuggestEngine.cpp` ↔ `PlaneQuerySuggestEngine.cpp` | TRUE clone → extract |
| **B** | 492 | `AnthropicClient.cpp` ↔ `OpenAiClient.cpp` (SSE-only — Ollama uses `AiNdjsonParser`, never in the clone set) | TRUE clone → extract |
| **C** | 415 / 377 | `AppController_LuaBindings.cpp` ↔ `AppController_LuaBindingsCore.cpp` | DELIBERATE (documented) → **deferred follow-up** |
| **D** | 337 / 269 / … | `JiraClient.h` ↔ `PlaneClient.h` ↔ `GitHubClient.h` | FALSE POSITIVE → `SMATCHET_DEVIATION` exempt (no extract) |

**Grill outcomes (cfce10b5 → this revision, 2026-06-07):** (1) **B scoped to Anthropic+OpenAi only** — Ollama uses `AiNdjsonParser` (different parser, different `Feed`/`Flush` arity, and missing the `LOG_ERROR` provider-visibility the SSE pair carries), so it is **not** in the confirmed clone and is dropped; the Ollama error-logging gap is a separate spawned follow-up. (2) **A helpers reparameterized to `const std::vector<TrackerField>&`** (not `const AppController&`) — tighter boundary + bucket-A-testable. (3) **A header home is `Source/Core/include/Tracker/`** alongside the engine headers (shared `QuerySuggestion` already lives in `include/QuerySuggestTypes.h`). (4) **D = in-plan `SMATCHET_DEVIATION` exemption**, no `dup_audit.py` logic change (the skip-region idea moves to the gate's calibration follow-up). (5) **C deferred** to a follow-up — not in this PR. (6) No CONTEXT.md term + no ADR (extraction is below glossary altitude; mechanical + reversible).

**Intended outcome — after this lands:** the two real clone clusters (A, B-as-scoped) are single-sourced; cluster D is exempted (not refactored) so it stops inflating the calibration sample; and the before/after clone counts are recorded as the first real WARN→block calibration data point for the DRY pillar gate. Prompted by an interactive review of the gate output (2026-06-07 session); no incident.

## Approach

**Cluster A — extract backend-agnostic query-suggest helpers (primary win).** Both engines open with a ~140-line anonymous-namespace block of *identical* string/catalog utilities that differ only by cosmetic identifier renames (`IsJqlIdChar`↔`IsPlaneIdChar`, `JqlQuotedValue`↔`QuotedValue`, `FindTrackerFieldForJqlToken`↔`FindFieldToken`, `AddSuggestionUnique`↔`AddUnique`, `AppendJqlTerms`↔`AppendTerms`, `AppendFieldCatalogSuggestions`↔`AppendFieldCatalog`, plus byte-identical `ScanStringStateToCursor` / `AsciiEqualsIgnoreCaseToLowered` / `AsciiStartsWithIgnoreCase` / `IsUserField` / `IsDateField`). None contain JQL-vs-Plane grammar — they are pure quoting / prefix-match / field-catalog iteration. Move them into a new `TrackerQuerySuggestCommon` TU — header in `Source/Core/include/Tracker/` (alongside the existing engine headers; it includes the already-shared `QuerySuggestTypes.h` where `QuerySuggestion` lives), impl in `Source/Core/src/Tracker/` — with one canonical name each; both engines `#include` it. **Reparameterize, don't verbatim-move**: the two field-catalog helpers currently take `const AppController&` only to call `app.GetAvailableFields()` (confirmed the sole `AppController` use in the shared block — `GetAvailableUsers()` is outside it). Change them to take `const std::vector<TrackerField>&` directly; each caller passes `app.GetAvailableFields()`. This decouples the shared helper from the `AppController` god-object (tighter boundary) and makes it bucket-A unit-testable without an `AppController` fixture. This is shared *implementation*, not a shared *interface*, so it does **not** violate the Tracker `AGENTS.md` "no backend leak into shared interfaces" invariant (it never touches `ITrackerBackend` or its role interfaces). The backend-specific grammar (operator/keyword tables, the `Suggest()` entry point, value-suggestion specifics) stays in each engine.

**Cluster B — extract the SSE drive skeleton behind a per-provider dispatch callback (Anthropic + OpenAi only).** The confirmed 492-token clone is **`AnthropicClient` ↔ `OpenAiClient`** — both `AiSseParser` / `text/event-stream`, with a byte-identical skeleton: `AiSseParser` setup → `cpr::WriteCallback` with the double cancel-check → `cpr::Post` → `NetworkUsageTracker::Record` → `Flush` → the cancel / transport-error / HTTP-status-redact / EOF-final blocks (the tail is byte-identical between the two, `"eof"` final included). The only per-provider variation is (1) the header map, (2) the event-translation function (`DispatchAnthropicEvent` vs `DispatchOpenAiDataLine`), (3) the log prefix, and (4) Anthropic's extra `pendingFinishReason` state — which is **fully contained inside `DispatchAnthropicEvent`** ([AnthropicClient.cpp:96-112](../../../Source/Core/src/AnthropicClient.cpp)) and captured by the translate lambda, so the shared skeleton never sees it. Introduce `Source/Core/src/AiSseStream.{h,cpp}` exposing one function — `StreamSseRequest(url, headers, body, cfg, translateFn, onDelta, onError, cancel, providerName)` — that binds `AiSseParser`, owns the skeleton, and takes `translateFn` (translate takes `AiSseParser::Event`) as a `std::function`. Each of the two clients shrinks to: build headers → define its translate lambda → call `StreamSseRequest`. **Ollama is explicitly out** — it uses `AiNdjsonParser` with a different `Feed`/`Flush` arity (extra `onParseError`) and lacks the SSE pair's `LOG_ERROR` provider-visibility; folding it would require a parser-type template + a behaviour change (the missing logging), neither justified by a clone the gate never flagged. Ollama's logging gap is a separate spawned follow-up.

**Cluster C — deferred follow-up.** C (`JsonToLuaImpl`/`LuaToJsonImpl`) is an *intentional* copy — `AppController_LuaBindingsCore.cpp:59` documents it ("own private copy … the no-ImGui isolation is the point"). Resolving it is a distinct judgment (a dependency-free leaf `LuaJsonConvert.{h,cpp}` — sol2 + nlohmann only, zero ImGui / AppController includes — *iff* it pulls nothing unwanted, **else** a `SMATCHET_DEVIATION(rule=duplication; reason=intentional no-ImGui isolation; …)` in place). To keep this PR focused on the confirmed A+B clones it is **deferred to a follow-up**, not shipped here.

**Cluster D — `SMATCHET_DEVIATION` exemption (no product change, no gate change).** D is the C++ override-signature symmetry that every concrete tracker client must repeat to satisfy the shared `ITracker*` role interfaces; coupling it via a shared base would reintroduce the banned monolithic `ITrackerClient` and couple independent backends (review **CRITICAL** per Tracker `AGENTS.md`). The in-plan action is the cheap, local one: annotate the three backend `*Client.h` header spans with `SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry; …)` so the FP stops inflating the calibration sample. The more general `dup_audit.py` header-override skip-region is a **better long-term class-fix but belongs in the gate's WARN→block calibration follow-up** (`process.md` `dry-pillar-dup-gate`), not this product PR.

## Files to modify

Grepped before naming: `git ls-files | grep -iE 'QuerySuggestCommon|StreamSse|AiSseStream|LuaJsonConvert'` → **no collisions** (names free).

Each cluster is one commit on `feat/dedup-clones`; all ship in **one PR** (PR-batching: one PR per feature, A+B+D are the dedup feature).

**Cluster A — slice 1 (Tracker strict zone):**
1. `Source/Core/include/Tracker/TrackerQuerySuggestCommon.h` (new) — declarations of the shared query-suggest helpers (`QueryValueNeedsQuotes`, `QueryQuotedValue`, `InsertForValueToken`, `ScanStringStateToCursor`, `FindTrackerField`, `AddSuggestionUnique`, `AsciiStartsWithIgnoreCase`, `AppendTerms`, `AppendFieldCatalog`, `IsQueryUserField`, `IsQueryDateField`). Includes `QuerySuggestTypes.h` (shared `QuerySuggestion`) + `TrackerFieldSchema.h` (`TrackerField`). Header home matches the engine headers in `include/Tracker/`.
2. `Source/Core/src/Tracker/TrackerQuerySuggestCommon.cpp` (new) — the bodies (canonical copy). **The two field-catalog helpers take `const std::vector<TrackerField>&`, not `const AppController&`** (decouple from the god-object; bucket-A-testable).
3. [`Source/Core/src/Tracker/JqlSuggestEngine.cpp:13`](../../../Source/Core/src/Tracker/JqlSuggestEngine.cpp) — delete the local helper block, `#include "Tracker/TrackerQuerySuggestCommon.h"`, re-point call sites (pass `app.GetAvailableFields()` where the helper now wants the vector).
4. [`Source/Core/src/Tracker/PlaneQuerySuggestEngine.cpp:13`](../../../Source/Core/src/Tracker/PlaneQuerySuggestEngine.cpp) — same.

**Cluster B — slice 2 (AI-source-gated zone; Anthropic + OpenAi only):**
5. `Source/Core/src/AiSseStream.h` (new) — `StreamSseRequest(...)` signature + the `TranslateFn` typedef (translate takes `AiSseParser::Event`).
6. `Source/Core/src/AiSseStream.cpp` (new) — the shared `AiSseParser`-bound cpr-drive + error skeleton.
7. [`Source/Core/src/AnthropicClient.cpp:156`](../../../Source/Core/src/AnthropicClient.cpp) — replace inline skeleton with a `StreamSseRequest` call; keep `DispatchAnthropicEvent` + the captured `pendingFinishReason` local.
8. [`Source/Core/src/OpenAiClient.cpp:156`](../../../Source/Core/src/OpenAiClient.cpp) — same.
9. [`CMakeLists.txt:908`](../../../CMakeLists.txt) — **add `AiSseStream.cpp` to `_SMATCHET_AI_SOURCES`** (the explicit list pruned from the glob then re-added only under `SMATCHET_WITH_AI`; a new AI-only TU left to the bare glob breaks the AI-OFF build). `TrackerQuerySuggestCommon.cpp` needs **no** CMake edit — picked up by the `file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "Source/Core/src/*.cpp")` at [`CMakeLists.txt:801`](../../../CMakeLists.txt). *(`OllamaClient.cpp` is NOT modified — out of scope per grill Q1.)*

**Cluster D — slice 3 (exemption, no logic change):**
10. [`Source/Core/include/Tracker/JiraClient.h`](../../../Source/Core/include/Tracker/JiraClient.h), [`PlaneClient.h`](../../../Source/Core/include/Tracker/PlaneClient.h), [`GitHubClient.h`](../../../Source/Core/include/Tracker/GitHubClient.h) — add `SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry; owner=<author>; revisit=<date>)` above each clone-flagged override-decl span. No `dup_audit.py` change (the skip-region class-fix is deferred to the gate calibration follow-up).

**Cluster C — deferred (NOT in this PR):**
- `LuaJsonConvert.{h,cpp}` leaf-or-exempt for `AppController_LuaBindings.cpp` ↔ `AppController_LuaBindingsCore.cpp` — its own follow-up; see § Out of scope.

## Existing utilities reused

- `ToLowerAsciiCopy` — [`Source/Core/include/StringUtil.h`](../../../Source/Core/include/StringUtil.h) — already the shared lower-case helper both engines call; the extracted helpers keep calling it (do not re-roll).
- `AiSseParser` — [`Source/Core/include/AiSseParser.h`](../../../Source/Core/include/AiSseParser.h) — the shared SSE line/frame parser; `StreamSseRequest` binds it and wraps the cpr feed loop around it (translate takes `AiSseParser::Event`), it is not re-implemented. (Ollama's `AiNdjsonParser` is a different parser — the reason Ollama is out of cluster B.)
- `smatchet::ai::pure::RedactProviderErrorBody` — already shared; the extracted error block calls it unchanged (no secret-leak regression).
- `NetworkUsageTracker::Instance().Record` — already the shared traffic accounting sink; called once inside `StreamSseRequest`.
- `QuerySuggestion` struct — [`Source/Core/include/QuerySuggestTypes.h`](../../../Source/Core/include/QuerySuggestTypes.h) — already the shared suggestion type both engines include; the new helper header includes it (no new type introduced).
- `TrackerField` / `TrackerFieldFamily` — [`Source/Core/include/Tracker/TrackerFieldSchema.h`](../../../Source/Core/include/Tracker/TrackerFieldSchema.h) — the field-schema vocabulary the reparameterized catalog helpers take directly (`const std::vector<TrackerField>&`), in place of `AppController`.

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
- **Strict Tracker lint zone (A).** `TrackerQuerySuggestCommon.cpp` lands in `Source/Core/src/Tracker/` (+ header in the matching `include/Tracker/`) — every new violation fails CI on a delta basis (narrowing, no-raw-new, func-size, func-branchy). *Mitigation:* moved code already passes today; keep functions intact (no merging that crosses the 120-line / 30-branch caps). The `AppController&`→`const std::vector<TrackerField>&` reparameterization is the one non-verbatim change — diff the resulting bodies against the originals to confirm only the parameter source changed.
- **Dual-target source gating (B).** Forgetting `AiSseStream.cpp` in `_SMATCHET_AI_SOURCES` breaks the `SMATCHET_WITH_AI=OFF` build (the bare glob would still include it). *Mitigation:* file #9 above is explicit; anchor `cmake --build … --target SmatchetStandalone SmatchetCore_DX12` and an AI-OFF configure to it.
- **Over-broad exemption (D).** A `SMATCHET_DEVIATION(rule=duplication)` placed too wide could mask a *future* real clone added inside the same header span. *Mitigation:* place each marker tightly above the specific interface-override-decl block the gate flags, not at file top; the `revisit=` date forces a re-look (and the deferred `dup_audit.py` skip-region is the durable class-fix).

**Non-goals:**
- No GitHub query-suggest unification (there is no `GitHubQuerySuggestEngine`).
- No unification of the per-provider event-dispatch logic (`DispatchAnthropicEvent` / `DispatchOpenAiDataLine` stay separate — that is the legitimate backend variation).
- **No Ollama change** — `OllamaClient` (NDJSON parser, divergent logging) is out of cluster B; its missing-`LOG_ERROR` gap is a separately-spawned follow-up.
- No coupling of the tracker backend clients via a shared base (cluster D) — explicitly forbidden.
- Does **not** graduate the DRY gate WARN→block, nor add the `dup_audit.py` header skip-region — both are the separate, calibration-gated follow-up in `process.md` (`dry-pillar-dup-gate`); this plan only *feeds* its sample.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest, `test-rig`)**: add `tests/Core/TrackerQuerySuggestCommon.test.cpp` exercising the extracted pure helpers (quoting round-trip, `ScanStringStateToCursor` string-state, prefix-match case-folding, field-catalog suggestion uniqueness). The `const std::vector<TrackerField>&` reparameterization (grill Q3) makes the catalog helpers testable with a hand-built field vector — **no `AppController` fixture needed**. No new pure surface for B (the skeleton is I/O-bound) beyond what `AiSseParser` tests already cover.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: existing `AiAssistantStreamingHappyPathScenario` + `AiAssistantStreamingTransportDownScenario` must stay green — they are the behaviour-equivalence proof for cluster B (cancel, transport-down, HTTP-error, EOF-final all routed through the new `StreamSseRequest`).
- **Bash-driver scenario / screenshot / sanitizer**: run the AI-streaming scenarios under the per-PR UBSan build (no new UB surface expected); no screenshot diff (no visual change).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) **plus** one `-DSMATCHET_WITH_AI=OFF` configure to prove `AiSseStream.cpp` gating is correct.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: **DONE 2026-06-07** (cfce10b5 → this revision). Outcomes: B re-scoped to Anthropic+OpenAi (Ollama is NDJSON, not in the clone); A helpers reparameterized to `const std::vector<TrackerField>&` (decoupled + testable); A header home corrected to `include/Tracker/`; D resolved as in-plan `SMATCHET_DEVIATION` (gate skip-region deferred); C deferred. No CONTEXT.md term + no ADR (below glossary altitude; mechanical + reversible) — verified against Tracker `CONTEXT.md` § Query, which already carries the "Suggest engine" term.
- **DRY gate confirmation**: after each cluster lands, `python agents/scripts/core/dup_audit.py --diff origin/develop` shows the corresponding clone tokens **gone** (and no new WARN introduced); record before/after clone counts as the calibration data point.
- **Manual residue**: none expected. If the cluster-C leaf decision ends up needing a human eyeball on include hygiene, file a `docs/self-improvement/categories/tooling.md` entry rather than leaving silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here (notably cluster C if deferred, and the cluster-D dup_audit skip-region), and revise or delete stale "deferred-as-current" refs.

- **Cluster C (Lua json↔lua) — deferred follow-up.** Not in this PR. Follow-up: a dependency-free `LuaJsonConvert.{h,cpp}` leaf *only if* it pulls zero unwanted includes, else `SMATCHET_DEVIATION` in place — respecting the documented no-ImGui isolation at `AppController_LuaBindingsCore.cpp:59`.
- **Ollama SSE-cousin + its logging gap** — `OllamaClient` is out of cluster B (NDJSON parser, divergent error-logging). Its missing transport/HTTP `LOG_ERROR` (the visibility Anthropic/OpenAi carry) is a **separately-spawned** product follow-up, not this refactor.
- **`dup_audit.py` header-override skip-region** — the durable class-fix for cluster-D-shaped header FPs; lives in the gate's WARN→block calibration follow-up (`process.md` `dry-pillar-dup-gate`), not this plan. This plan uses the cheap per-span `SMATCHET_DEVIATION` instead.
- **DRY WARN→block graduation** — separate calibration-gated follow-up; no-action here beyond feeding the before/after-clone-count sample.
- **The other ~445 baseline clones** — not triaged in this plan; addresses only the top TRUE-clone clusters. No-action until the gate graduates or another review pass triages them.
- **Tracker backend base-class extraction (cluster D product code)** — explicitly never; would violate the no-backend-leak invariant.

## Implementation log
- **Cluster A** · extracted `Source/Core/include/Tracker/TrackerQuerySuggestCommon.h` + `src/Tracker/TrackerQuerySuggestCommon.cpp` (13 backend-agnostic query-suggest helpers in `namespace tracker_query_suggest`; canonical names confirmed by a line-by-line diff of both engines' top blocks). The two field-catalog helpers (`FindTrackerField`, `AppendFieldCatalog`) **reparameterized `const AppController&` → `const std::vector<TrackerField>&`** (the sole prior `AppController` use was `GetAvailableFields()`) — decouples them from the god-object and makes them bucket-A-testable. `JqlSuggestEngine.cpp` + `PlaneQuerySuggestEngine.cpp` delete their local blocks, `#include` + `using`-import the canonical names, and pass `app.GetAvailableFields()` to the catalog helpers (**−336 net lines**, pure behaviour-preserving extraction). New `tests/Core/TrackerQuerySuggestCommon.test.cpp` (12 cases / 66 assertions, bucket-A) + its entry in `tests/CMakeLists.txt`. Routed to `tracker-backend`.
- **Cluster D** · `SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry…)` on the flagged override-decl spans of `Tracker/{JiraClient,PlaneClient,GitHubClient}.h` (Jira ×4 / Plane ×4 / GitHub ×3) — all 10 product-client-header clone pairs now suppressed; no `dup_audit.py` logic change (the gate-side skip-region remains the calibration follow-up).

## Deviations from plan
- **Cluster B deferred — NOT shipped (cost/value call, orchestrator + user 2026-06-09).** The plan bundled A+B+D in one PR; B (the `AnthropicClient`↔`OpenAiClient` SSE-skeleton extraction into `AiSseStream`) was dropped because it is a 492-token clone in **user-facing AI-chat streaming error handling** (cancel / transport-error / HTTP-redaction) — a botched extraction risks breaking streaming for *zero* behaviour change, the worst regression-risk-to-DRY-value ratio in the plan, against grandfathered WARN-only clones that have caused no incident. B's premises were triple-checked as still valid (both clients still carry the byte-identical skeleton; `pendingFinishReason` still Anthropic-contained inside `DispatchAnthropicEvent`; Ollama still `AiNdjsonParser`-out-of-scope), so B stays a clean future follow-up if the AI-client code is touched for other reasons.
- **Cluster C deferred — as originally planned** (intentional no-ImGui-isolation Lua-json copy; `LuaJsonConvert`-leaf-or-exempt is its own judgment).
- **2 new dup residuals exempted (extraction fallout, not anticipated).** Unifying the helper names removed the cosmetic-identifier difference the gate keyed on, so the now-identical `using`-block and the name-identical `AppendValueSuggestions` near-twin surfaced as fresh clones. Both `SMATCHET_DEVIATION`-exempted: the `using`-block is an unavoidable share artefact, and `AppendValueSuggestions` carries a genuine `(display name)` (Jira) vs `(display)` (Plane) divergence the plan kept per-engine — extracting it would collapse a real difference (Pillar-3 hazard). Gate is WARN-only so neither blocks.
- **`IsQueryDateField` shared though Plane had no date helper** — trivial pure family predicate, no grammar; kept on the canonical name.

## Verification (actual)
- **Cluster A**: new bucket-A suite **12 cases / 66 assertions PASS**; dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) **clean**; pure extraction (−336 net lines, no logic change) — behaviour-preserving.
- **Cluster D + the 2 residuals**: `python agents/scripts/core/dup_audit.py --diff origin/develop` → **0 `[dup] WARN`** (cluster-A helper clones gone; all client-header override symmetry + the 2 extraction residuals suppressed).
- Strict-zone delta-lint **clean**; `clang-format` applied. `tests-out-of-band` N/A (A ships with its test); `cr-out-of-band` (CodeRabbit org-credit/rate-limit).
- **Not done (deferred, see Deviations)**: cluster B, cluster C.

## Archive
Shipped + archived 2026-06-09: clusters A (TrackerQuerySuggestCommon extraction) + D (client-header dup exemptions) landed; B (AiSseStream) + C (LuaJsonConvert) deferred-with-rationale (§ Deviations) and tracked in `docs/self-improvement/categories/debt.md`.

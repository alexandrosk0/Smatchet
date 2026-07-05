# PERF_WIN_HUNT.md — offensive perf-win hunt (2026-07-05)

> Companion to the **defensive** perf system (`docs/plans/shipped/pillar-1-2-perf-review-system.md`,
> `pillar-1-2-audit-2026-05-17.md`) which *gates* regressions. This session *hunts wins*.
> **Environment constraint:** the hunt ran on a Windows/MSYS host that cannot exercise the app's
> runtime behaviour in a way that produces trustworthy numbers, so **every runtime claim here is
> UNVALIDATED until a CI perf lane confirms it**. `perf-pr-fast.yml` (per-PR) + `perf-full.yml`
> (nightly) are the *only* acceptable gate for a perf-motivated change (`regression-policy.json`
> noise floor: `mean_min_abs_delta_ms = 0.05` ≈ 0.7 % of the 6.94 ms frame budget). Nothing below is
> asserted as a proven win without a cited CI number.

## Phase 0 — overlap verdict (explicit)

**Track A (build velocity) is DROPPED.** Build-velocity is already an *active, claimed, and
substantially-executed* workstream in `docs/plans/active/build-quality-velocity-hardening.md`
(Sprint 2 "Root multiplier A — the header choke" + throughput items). The highest-ROI structural
levers are **shipped and measured**:

- **pImpl `AppController` (#19a/b/c)** — sol2-dependent objects **197 → 9**; `AppController.h` now
  carries `<nlohmann/json_fwd.hpp>` (verified live on develop), not the full json/sol2/`JiraClient`
  headers. The ~100 Ui/Commands includers no longer compile `<sol/sol.hpp>`.
- **Core_DX12 json-PCH (#20/#28)** — measured **13.7 % faster** (32.85 s → 28.34 s isolated
  target compile), KEEP; `Source/Core/include/SmatchetPchCoreDx12.h` live.
- **Measured NO-GO verdicts already recorded** (do not re-propose): Standalone PCH (net-wash),
  ghc-in-PCH (6–7 % reach), `json_fwd` swap in isolation (only 4/298 TUs), serial-configure split
  (net-negative overhead).

Re-hunting build-time here would collide with that plan's territory and re-litigate its measured
decisions. Per the session brief's pre-authorised conditional ("*if build-time work is already
claimed by the active plan, drop Track A and expand Track B*"), Track A is dropped and Track B is
the primary deliverable.

### Track-A residue (genuinely NOT covered by the active plan — pre-scoped follow-ups)

The active plan did *targeted-hub* build-time work driven by a static audit, not a profiling-driven
sweep. These three are non-overlapping and routed to that plan's owner, not executed here:

- **A-R1 · `-ftime-trace` CI-aggregation harness.** No systematic per-header/per-TU compile-time
  profile exists. Add a `-ftime-trace` build leg (the Linux `ninja-tsan-linux` / `posix-core-check`
  clang presets already exist) + a `ClangBuildAnalyzer`-style aggregation step that reports the
  top-N most expensive headers/TUs per run. This turns "which header hurts" from a static guess
  (the audit) into a measured ranking — and would confirm or refute the audit's hub picks.
- **A-R2 · Unity/jumbo-build trial for `Source/Core/src/Commands/Scenarios/` + `Tracker/`.** These
  are many small sibling TUs sharing near-identical include preambles (confirmed by the existing
  `duplication`-exempt "idiomatic per-TU ImGui-localization preamble" clones). A scoped unity build
  amortises the shared-header parse across the group. **NO-GO-in-place risk:** unity builds can
  break ODR/anonymous-namespace assumptions and the `#define ImGui SmatchetLocalizedImGui` per-TU
  idiom — a *measurement* (CI wall-clock), not a commitment.
- **A-R3 · Forward-declaration sweep of the *next*-tier hubs.** The audit fixed the #1 hub
  (`AppController.h`). The next tier — headers included transitively "everywhere" after that — was
  never enumerated. A-R1's output is the prerequisite (measure before cutting).

## Track B — static hot-path hunt (primary deliverable)

**Map:** `docs/perf/MARKER_INVENTORY.md` (the `SMATCHET_UI_PERF_SCOPE` census). The per-frame hot
paths are `SmatchetUI::Draw` → grid (`activeProject:grid.rows/setup/sort`, `RenderFieldCell`,
`TrackerGridFieldDisplay::Render*Field`) and AI chat (`ai_chat.history.render_turn/.draw`). **Gate
scenarios** (the ci-windows-latest baselines that can validate a Track-B change): `priority-grid-scroll`,
`side-by-side-2-grid`, `ai-chat-history-render`, `cell-edit-burst`, `idle`, `concurrent-sync`.

**Codebase maturity caveat (shapes every ranking below):** the render hot paths are *already*
perf-hardened — description tooltips parse ADF→markdown lazily on hover only
(`TicketFieldEditor.cpp:1352`), the special-field render models (attachments/watchers/votes/worklog/
progress) are memoised via `GetOrBuildCachedValue(cache, currentValue, …)`
(`TrackerGridFieldDisplay.cpp:645/675/…`), the sort+filter projection is a dirty-flagged cache with
a 500 ms streaming debounce (`SmatchetActiveProjectGridUi.cpp:1149`), and per-frame-alloc removal
already happened once in the render path (**PR #1600**, `SmatchetOmnibarUi.cpp` — a compare-guarded
`std::string::assign`). So the remaining wins are **incremental**, not structural — ranked honestly
below by **(frequency × cost × fix safety)**.

### B1 — `ResolveDisplayValue` per-cell-per-frame string allocation (grid) · TOP finding · FOLLOW-UP (correctness-gated)

- **Where:** `RenderPlainTextCell` (`TicketFieldEditor.cpp:1345`) calls
  `app.ResolveDisplayValue(fieldId, field, currentValue)` → `backend->Reader().ResolveDisplayValue(...)`
  (`AppController_CatalogAndFieldEdit.cpp:227`), which **returns a `std::string` by value** (option-id
  → display-name map resolution + a heap string copy) for **every plain-text cell, every frame**.
  Same call in the collapsed-editor previews (`TicketFieldEditor.cpp:627/739/757/782/895/924`).
- **Frequency × cost:** HIGHEST of the findings — `rows × text-columns` per frame during
  `priority-grid-scroll` / `side-by-side-2-grid`. Cost per call = a map lookup + a `std::string`
  allocation. This is the only finding that plausibly clears the 0.05 ms noise floor on a real grid.
- **Fix:** memoise the resolved display string exactly like the sibling special fields already do —
  `GetOrBuildCachedValue(cache, key, …)` keyed by `(fieldId, currentValue)`, invalidated on
  `app.GetFieldCatalogRevision()` (the grid *already* tracks this revision at
  `SmatchetActiveProjectGridUi.cpp:1123` for its sort cache, so the invalidation signal exists).
- **Why FOLLOW-UP, not a PR here:** the `components` field resolves against **per-project options
  that load asynchronously** (`TicketFieldEditor.cpp:1330–1344`) — a naive `(fieldId,value)` cache
  returns a stale raw-id display until the async load lands and would *not* self-invalidate on the
  options arriving (the catalog revision may not bump for a per-project component fetch). So the
  cache's **correctness is not validatable by the perf lane alone** — it needs a bucket-E functional
  assertion that a components cell still resolves its name after a project-options load. Per the
  escalate-when-unvalidatable charter, a change whose correctness the available gate can't confirm
  is not shipped blind. **Scoped follow-up:** cache only the non-`components` path (the common case),
  OR add the bucket-E display-correctness assertion first, then cache all paths. Routed to
  `grid-engine` / `tracker-backend`.

### B2 — unconditional per-frame `std::string::assign` in the AI input (the #1600 class) · PR THIS SESSION

- **Where:** `SmatchetAiAssistantUi.cpp:1130` — `d.assistantInputBuf.assign(s_inputCharBuf.data())`
  runs **every frame** the assistant panel is open ("*Mirror char-buf back into the string field
  every frame*"), copying (and potentially reallocating) the input string even when nothing was
  typed. This is byte-for-byte the pattern PR #1600 removed from the omnibar.
- **Frequency × cost:** LOW-MODERATE — once per frame (not per-cell), string ≤ 8 KB. Absolute cost
  is small; whether it clears the noise floor is exactly what `ai-chat-history-render` will tell us.
- **Fix (dead-safe, one line):** guard with the allocation-free `std::string != const char*`
  compare, skipping the copy in the steady state — identical to #1600:
  `if (d.assistantInputBuf != s_inputCharBuf.data()) d.assistantInputBuf.assign(s_inputCharBuf.data());`
- **Safety:** MAXIMAL — no behaviour change (the field holds the same value; only a redundant copy is
  skipped), matches an already-accepted precedent, single line. **This is PR #1 (below); its CI
  perf-pr-fast number is cited on the PR.** If the number is within noise, that is reported honestly
  as a null result — the change is still correct and removes a real redundant allocation, but is not
  claimed as a measured win.

### B3 — `components` cell copies a whole `TrackerField` per-frame-per-cell · FOLLOW-UP

- **Where:** `RenderPlainTextCell` (`TicketFieldEditor.cpp:1341–1343`) does `effectiveField = *field;`
  (copies the `TrackerField`, including its `AllowedValueOptions` vector) then
  `effectiveField.AllowedValueOptions = std::move(perProject);` — for **every `components` cell every
  frame**, plus `GetComponentOptionsForProject` returns a fresh `std::vector` copy each call.
- **Frequency × cost:** MODERATE — only `components` columns (uncommon), but a full-field + vector
  copy per such cell per frame is heavier than B2.
- **Why FOLLOW-UP:** the fix (resolve per-project options once per (project,frame) instead of per
  cell, or pass options by pointer) is more than a one-liner and shares the B1 correctness surface
  (per-project async load). Bundle with B1.

### B4 — per-frame button-ID `std::string` allocation on watchers/votes cells · DOCUMENT-ONLY

- **Where:** `TrackerGridFieldDisplay.cpp:688/724/771` — `const std::string loadBtn = "Load##watch_"
  + issueKey;` (and `"Watch##wself_"`, `"Load##votes_"`) rebuilt every frame per visible
  watchers/votes cell (a heap alloc + concat for an ImGui widget ID that is constant per cell).
- **Assessment:** provably-redundant and a clean fix (stack `char[]` + `snprintf`, or cache the ID),
  but watchers/votes are **rare grid columns** absent from the standard gate scenarios, so a PR
  would add diff with **no measurable coverage**. Document-only unless a watchers/votes scenario is
  added. Low priority.

### B5 — sort `fingerprint` string rebuilt per-frame · DOCUMENT-ONLY (load-bearing)

- **Where:** `SmatchetActiveProjectGridUi.cpp:1128–1142` builds a `std::string fingerprint` (with
  `std::to_string` per sort spec) **every frame** to compare against `pane.cachedSortFingerprint`
  and drive projection-cache invalidation.
- **Assessment:** it is `reserve`-d and small (sort specs are typically 1–3), and it is the
  **load-bearing dirty-check** for the whole projection cache — replacing it with an integer
  fingerprint is a correctness-sensitive change for a sub-microsecond saving. Not worth the risk;
  documented for completeness.

## Ranked summary

| # | Finding | Freq × Cost | Fix safety | Disposition |
|---|---|---|---|---|
| **B1** | `ResolveDisplayValue` per-cell string alloc (grid) | **High** | Medium (correctness-gated) | **Follow-up** — needs bucket-E display-correctness assertion; biggest potential win |
| **B2** | AI-input per-frame `.assign` (#1600 class) | Low-Mod | **Max** | **PR this session** — ride `ai-chat-history-render`, cite number |
| **B3** | `components` cell per-frame `TrackerField` copy | Moderate | Medium | Follow-up (bundle with B1) |
| **B4** | watchers/votes per-frame button-ID strings | Low | High | Document-only (no gate coverage) |
| **B5** | sort fingerprint per-frame string | Very low | Low (load-bearing) | Document-only |
| **A-R1/2/3** | ftime-trace harness / unity trial / next-tier fwd-decl | (build-time) | — | Follow-up → `build-quality-velocity-hardening.md` |

## What ships this session

- **1 PR** (cap 2, using 1): **B2** — the dead-safe #1600-class per-frame-alloc guard, whose only
  perf claim is the number the `perf-pr-fast` lane returns.
- Everything else is a **pre-scoped follow-up** above (B1/B3 → grid/tracker; B4/B5 → document-only;
  A-R1/2/3 → the active build-velocity plan). This split honours the "no speculative
  micro-optimization without a number attached" constraint: the codebase is mature enough that the
  honest finding is *"one dead-safe win worth landing now; the biggest potential win (B1) is
  correctness-gated and must be validated functionally, not just for perf."*

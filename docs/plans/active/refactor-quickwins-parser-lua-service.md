# Plan — Refactoring wave: dup-baseline quick wins + FieldValueParser split + AppController Lua-script-library service

> **Slug**: `refactor-quickwins-parser-lua-service`
>
> **Status**: `active`
>
> Saved for later execution — planned in a refactoring-discussion session (2026-07-19); implementation deferred by user request. All slices land on one branch / one PR (related slices batch per `AGENTS.md` § Autonomous ship-loop default).

## Context

A "what would you refactor?" survey cross-referenced the debt records (`docs/self-improvement/categories/debt.md`, `docs/high-integrity/dup-baseline.md` — 449 grandfathered clones) with an independent source scan. Three approved work streams: (1) dup-baseline burn-down quick wins + the two remaining P3 debt items, (2) `TrackerFieldValueParser.cpp` decomposition, (3) continuing the AppController god-object decomposition (debt.md P2, "partially applied") with the next cleanly-bounded slice. After this lands: the P3 debt entries close, the dup baseline shrinks by ~15 grandfathered clone pairs, `TrackerFieldValueParser.cpp` drops from 1,101 lines to cohesive companion TUs, and the sol-free Lua script/consent cluster leaves `AppController` behind a narrow `*Deps` interface.

Facts verified during planning (they rescope the naive version of this work):
- Hardening #19c already moved every sol-typed Lua binding onto `AppController::Impl` behind `ILuaBindingHost` (`Source/Core/include/ILuaBindingHost.h`); `docs/plans/god-file-splits.md` already split the binding TU. The remaining extractable Lua debt is the **sol-free script-file + consent cluster** (`Source/Core/src/AppController_LuaScriptFiles.cpp`, 351 lines).
- `LuaAutomationHost` is deliberately a 43-line log-sink coordinator (B1 reframe, `docs/plans/lua-host-friend-drop.md`) — do NOT grow it; a new service is the vehicle.
- `TrackerFieldValueParser.cpp` is ~35 small free functions (baseline-clean on function caps), not one dispatcher — the refactor is a TU split plus table-izing the object-normalization chain, not a monolithic registry.
- Sequencing hazard: the in-flight #21 `Result`/`VoidResult` wave rewrites field-edit/edit-meta signatures. This plan avoids those seams (`ApproveLuaScript`/`RevokeLuaScript` already flipped to `VoidResult` in cd890b5, so their signatures are stable).

## Approach

Four sequenced, independently-buildable commits, lowest-risk first. Commits 1–2 are mechanical dedup/inversion quick wins; commit 3 is a behavior-preserving TU partition per the `docs/plans/god-file-splits.md` convention; commit 4 mirrors the shipped five-service extraction template (`ConnectivityMonitorService` as the model: verbatim body moves, narrow `I*Deps` interface, header-only test fake, thin delegators so the ~113 `AppController.h` includers see zero change).

Trade-off named once: the Lua slice is scoped to the sol-free cluster only — extracting the console/actions/MCP-execute/DrawLuaWindows surface would move sol2 state ownership off `AppController::Impl`, which the B1 reframe settled differently; that is a separate future plan.

### Commit 1 — `refactor(dedup)`: dup-baseline burn-down

Replace re-inlined helper copies with calls to the canonical header helper (baseline entries at `docs/high-integrity/dup-baseline.md:16-18,41-59`):
- `Source/Core/include/Tracker/TrackerError.h:123-129` helpers re-inlined at `Command.cpp:9`, `CodeColorView.cpp:563`, `AiEndpointSanitize.cpp:190-192`, `HotkeyParse.cpp:158` — delete the local copy, include the header, call it. Guard: if the include would violate the DAG-ified include-graph gates (#1282), leave that site (grandfathered) and note why in the commit message.
- `Source/Core/include/StringUtil.h:8-9` helpers re-inlined at `IssueDraft.cpp:291`, `TicketGridModel.cpp:97`, `TrackerFieldValueParser.cpp:979` — same treatment.
- `Source/Core/src/Ui/SmatchetTheme.cpp:541-547` ↔ `Source/Core/include/Ui/SmatchetTheme.h:78-82` — theme-token list declared and defined in parallel (~8 clone pairs). Drive both from one X-macro list (`SMATCHET_THEME_TOKENS(X)`); token order/values byte-identical so rendered output cannot change.

### Commit 2 — `refactor(core)`: remaining P3 debt items

- **Sanitizer field list** (debt.md P3): `Source/Core/src/Config/ConfigManager.cpp` hand-overwrites `j["ai_base_url"]` / `j["ai_ollama_base_url"]` / `j["ai_deepseek_base_url"]` with `SanitizeConfigStringValue(...)` (~:376-378) separately from the persist field table (~:229). Introduce one shared sanitized-URL-field table (json key ↔ `TrackerConfig` member pointer, same idiom as the :229 table) consumed by both the persist-time sanitizer and the `Build*ClientConfig` path, so a new provider is auto-covered. Doctest: every `ai_*_base_url` key in the persist table is sanitize-covered.
- **`PluginHost::SetMcpPluginFactory`** (debt.md P3 item 2; ADR-0002-governed — re-read the ADR before implementing): `Source/Core/src/PluginHost.cpp:124` constructs `make_unique<McpPlugin>(port)` by name. Add `void PluginHost::SetMcpPluginFactory(std::function<std::unique_ptr<IPlugin>(int port)>)`; `SyncMcpPluginWithConfig` uses the factory when set. Wire from the bootstrap that registers plugins (mirror the `ITrackerBackendFactory` pattern). Keep existing build gating; behavior identical.

### Commit 3 — `refactor(tracker)`: `TrackerFieldValueParser` split + normalize table

1. Partition `Source/Core/src/Tracker/TrackerFieldValueParser.cpp` into companion TUs (same public header `Source/Core/include/Tracker/TrackerFieldValueParser.h`, zero API change): `_Options.cpp` (option/id/display: `BuildTrackerOption*`, `TrackerFieldOptionFromJson`, `MergeTrackerFieldOption`, `RefreshTrackerAllowedValuesFromOptions`, `ClassifyTrackerFieldFamily`), `_CommentsAdf.cpp` (`ParseComments`, `ParseCommentAuthor`, ADF walkers + depth guards), `_Changelog.cpp` (`ParseChangelog*`, `Changelog*`), `_Duration.cpp` (`ParseWorkDurationToSeconds`, `FormatWorkDurationFromSeconds`, `FormatTrackerTimetrackingDisplay`, `JsonLooksLikeJiraTimetracking`); core file keeps the small json/string utilities + `NormalizeTrackerFieldValue*`. Shared file-local helpers (`TrimTrailingZeros`, `SafeJsonDump`, depth guards) move to a `TrackerFieldValueParser_detail.h` so the split introduces no new clone (DRY gate). `TrackerFieldValueParser_detail.h` must also declare `JsonLooksLikeJiraTimetracking` (definition in `_Duration.cpp`): `NormalizeTrackerObjectValue` — which stays in the core file — calls it at `TrackerFieldValueParser.cpp:1007`, so the symbol needs cross-TU visibility after the split; `FormatTrackerTimetrackingDisplay` is already declared in the public header and needs nothing.
2. Table-ize `NormalizeTrackerObjectValue` (`TrackerFieldValueParser.cpp:950`): the sequential `bool& handled` probe chain (`NormalizeParentRefObject` :897, `NormalizeIdObject` :927, …) becomes an ordered C++14 function-pointer array `{ const char* name; std::string (*fn)(const nlohmann::json&, bool& handled); }` iterated first-match-wins. Identical output.
3. Update the `Source/Core` CMake source list for the new TUs.

### Commit 4 — `refactor(app)`: `LuaScriptLibraryService` extraction

- New `Source/Core/include/LuaScriptLibraryService.h` + `Source/Core/src/LuaScriptLibraryService.cpp`: owns script-dir resolution, file listing, and the first-run consent gate. Methods moved verbatim from `AppController` (bodies byte-for-byte where possible — behavior-preservation contract): `ResolveLuaScriptPath`, `ListLuaScriptFiles`, `IsLuaScriptConsented`, `ApproveLuaScript`, `RevokeLuaScript`, `ListApprovedLuaScriptPaths`, `SeedLuaScriptConsentIfNeeded` (all implemented today in `Source/Core/src/AppController_LuaScriptFiles.cpp`).
- New narrow `ILuaScriptLibraryDeps` (mirror `Source/Core/include/IConnectivityDeps.h`): only what those bodies actually reach (config/paths + consent-store access — pin down exactly while reading the TU). Implement on `GridContextDepsAdapter` alongside the five existing interfaces if the deps overlap, else a tiny dedicated adapter.
- `AppController` keeps its full public surface (incl. the `IAppAutomation` facet overrides) as thin one-line delegators; service constructed in `WireCoreServices()`, owned via `unique_ptr`, destroyed before the deps adapter (same lifetime contract as the sibling services).
- Header-only fake (`tests/support/` pattern) + doctest unit for the consent gate (approve → consented; revoke → not; seed idempotence).
- Update the debt.md P2 residual note (Lua residue shrinks to console/actions/MCP-execute/DrawLuaWindows).

## Files to modify

1. `Source/Core/src/Commands/Command.cpp:9`, `Source/Core/src/Ui/CodeColorView.cpp:563`, `Source/Core/src/Ai/AiEndpointSanitize.cpp:190`, `Source/Core/src/HotkeyParse.cpp:158` — call `TrackerError.h` helpers instead of local copies (paths approximate — locate via the dup-baseline hashes).
2. `Source/Core/src/IssueDraft.cpp:291`, `Source/Core/src/TicketGridModel.cpp:97`, `Source/Core/src/Tracker/TrackerFieldValueParser.cpp:979` — call `StringUtil.h` helpers.
3. `Source/Core/src/Ui/SmatchetTheme.cpp:541` + `Source/Core/include/Ui/SmatchetTheme.h:78` — X-macro token list.
4. `Source/Core/src/Config/ConfigManager.cpp:229,376` — shared sanitized-field table + doctest.
5. `Source/Core/src/PluginHost.cpp:124` + `Source/Core/include/PluginHost.h` + plugin bootstrap site — factory inversion.
6. `Source/Core/src/Tracker/TrackerFieldValueParser.cpp` → four new companion TUs + `_detail.h` + CMake source list (commit 3; new-TU names grepped for collisions first).
7. `Source/Core/include/AppController.h`, `Source/Core/src/AppController_LuaScriptFiles.cpp`, `Source/Core/src/AppController_Init.cpp` (`WireCoreServices`), `Source/Core/src/GridContextDepsAdapter` impl — plus new `LuaScriptLibraryService.{h,cpp}`, `ILuaScriptLibraryDeps.h`, test fake + doctest (commit 4).

## Existing utilities reused

- `SanitizeConfigStringValue` — `Source/Core/src/Config/ConfigManager.cpp:37` (via `smatchet::config_detail`) — the sanitizer itself is unchanged; only its application becomes table-driven.
- `LuaScriptConsent.h` pure core — `Source/Core/include/LuaScriptConsent.h` — consent logic already extracted pure; the service wraps it, not reimplements it.
- Five-service extraction template — `Source/Core/include/ConnectivityMonitorService.h` / `Source/Core/include/IConnectivityDeps.h` — lifetime/threading contract style, adapter wiring, fakes-for-tests.
- `ITrackerBackendFactory` pattern — model for `SetMcpPluginFactory`.
- Companion-TU convention — `docs/plans/god-file-splits.md`.
- Existing behavior pins: `tests/Core/TrackerFieldValueParser.test.cpp` + `tests/Core/TrackerFieldValueParser.extended.test.cpp`, `scripts/dev/test-theme-roundtrip.sh`, `scripts/dev/test-theme-syntax-colors.sh`.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

- Commit 3 (TU split): source `TrackerFieldValueParser.cpp` 1,101 lines → core file projected ~300 lines; four companion TUs ~150–250 each + small `_detail.h`. All well under the advisory tu-line-ceiling; no doc caps involved.
- Commit 4 (service extraction): `AppController_LuaScriptFiles.cpp` (351 lines) EXTRACT → `LuaScriptLibraryService.cpp`; STAYS on `AppController`: one-line delegators + facet overrides (~30 lines). `AppController.h` net-shrinks by the moved decls' doc comments (~40 lines).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — behavior-preserving moves and table lookups on cold paths (config persist, script consent, field-value parse already off the per-frame path).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — no new I/O; existing sync behavior unchanged.
- **Pillar 3 (never crash)**: guarded — verbatim body moves + pinned doctest/roundtrip suites; the normalize table preserves probe order exactly; sanitizer build stays green.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no impact — no UI behavior change; theme token values byte-identical.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — fires: grid/field-display scenarios cover the parser + theme paths; no new scenario needed (no behavior change).
2. **Pillar 2 static scanner** — N/A: no new sync-I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new sync-stall path.
5. **Marker inventory** — N/A: no new `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against the grid-render scenario before opening the PR.

## Risks / non-goals

- #21 `VoidResult` wave collision — mitigated: `SubmitFieldEdit` / `EnsureIssueEditMetaLoaded` / `RefreshIssueEditMeta` and all field-edit pipeline seams untouched; rebase on `origin/develop` before starting commit 4.
- `SmatchetTheme.cpp` is on the visual-validation exception list — mitigated: token list order/values byte-identical + roundtrip/syntax-color tests; any visual doubt → pause for user verification per `AGENTS.md`.
- DRY refactor coupling independent subsystems = review CRITICAL — mitigated: clone fixes only call headers already reachable in the DAG-ified include graph; a site that would need a new cross-subsystem edge stays grandfathered.
- Non-goal: growing `LuaAutomationHost` or moving sol2 state ownership off `AppController::Impl` (B1 reframe stands).
- Non-goal: extracting the AI-assistant / MCP-activity concerns from `AppController` (separate future plans; debt.md residual updated instead).

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: full doctest rig green each commit (`ninja-test-*` preset for the platform; `ninja-test-linux` in a Linux container); parser suites pass unchanged; new doctests for sanitizer-table coverage + consent gate.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: existing UI suites only — no new bucket-E coverage needed (no UI behavior change); theme roundtrip covered by the bash drivers below.
- **Bash-driver scenario / screenshot / sanitizer**: `bash scripts/dev/test-theme-roundtrip.sh` + `bash scripts/dev/test-theme-syntax-colors.sh` after commit 1; sanitizer preset build after every commit of the wave (covers commit 2's `ConfigManager` sanitizer-coverage change and commit 4's service extraction alike).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Lint/DRY gates**: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` + `agents/scripts/core/dup_audit.py --diff origin/develop` — commits 1–2 must show baseline clone removals, never additions; full `bash scripts/dev/pre-ship.sh` before push.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome. Outcome so far: planning-session verification already rescoped the Lua slice (sol-typed bindings found pre-extracted) and the parser design (35 small functions, not one dispatcher) — re-grill at implementation start after rebasing.
- **Manual residue**: none — all steps above are scripted.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Lua console/actions/MCP-execute/`DrawLuaWindows` extraction — follow-up plan (touches sol2 ownership on `Impl`).
- AI-assistant + MCP-activity `AppController` concerns — separate plans per debt.md residual note.
- Tracker backend client dedup (`JiraClient` ↔ `PlaneClient` ↔ `GitHubClient` clone cluster) — surveyed, deliberately not in this wave (touches wire behavior of every backend); candidate for its own plan.
- `Initialize`-time `HostCallbacks` injection (vs the retained setters) — deferred follow-up already tracked in debt.md.

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

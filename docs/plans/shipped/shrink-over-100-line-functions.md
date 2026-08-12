# Plan — Shrink every function over 100 lines under the soft-warn tier
<!-- plan-date: 2026-06-06 -->

> **Slug**: `shrink-over-100-line-functions` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

<!-- index-summary: Decompose all 68 functions over the 100-line soft-warn tier (UI + non-UI) under 100 lines, ideally 40-80. Follow-up to decompose-top-20-monoliths (which cleared the hard cap). -->

## Context

`decompose-top-20-monoliths` (shipped) cleared every function over the **hard** cap (120 non-UI /
200 ImGui-draw / 30 branches) — the live baseline is now **0 grandfathered entries**. What remains is
the **soft-warn tier**: `function_size_audit.py` emits a non-blocking `[func-size] WARN` for any
function over **100 lines** or **20 branches**, nudging toward the 40-80-line ideal. A live tree-wide
scan (2026-06-06) finds **132** soft-warn functions: **68 over 100 lines** and **64 over 20 branches only**.

This plan addresses **the 68 over-100-line functions only** (maintainer decision 2026-06-06). The
64 branch-only functions are **explicitly out of scope** — reducing decision count is genuine logic
restructuring (table-dispatch, early-return flattening), not the mechanical section-extraction this
plan applies, and carries materially higher regression risk in strict-zone parsers. They stay as
non-blocking advisories.

**Intended outcome — one sentence:** after this lands, no function in the tree exceeds 100 lines (most
land in the 40-80 ideal), achieved by behaviour-preserving extraction, verified per batch by
dual-target build + the relevant bucket-E / ctest coverage, with zero visual or behavioural delta.

## Approach

Behaviour-preserving extraction — **never** a logic rewrite. Two recipes by call-site shape:

- **UI-draw functions** (`Ui/` path or `Draw`/`Render` name) → the section-helper pattern from
  [`imgui-draw-pattern.md`](../../guides/imgui-draw-pattern.md): `DrawCtx` struct, section helpers at
  existing `SMATCHET_UI_PERF_SCOPE` seams (reuse verbatim → zero baseline shift), `static` locals →
  `<Foo>WindowState` member, action handlers → `OnX()`. Positional-ImGui pairing preserved byte-for-byte.
- **Non-UI functions** (parsers, payload builders, config loaders, HTTP runners) → extract cohesive
  phases/loops into named free helpers (anon-namespace where file-local), or table-driven dispatch
  where an if/else tower maps key→handler. Pure helpers go to a `*Pure`/`*_detail` seam where one
  exists so they become bucket-A testable.

**The baseline is regenerated ONCE at campaign end** (Rule 7) — never per batch (cross-PR cascade).

## Batches (one PR each; build + tests per PR)

| Batch | Scope | Count | Zone | Verification |
|---|---|---|---|---|
| **A — Preferences UI** | `drawPreferencesTrackerTab`, `onPreferencesSaveAndSync`, `DrawLocalDataTab`, `DrawAppearanceTab`, `DrawQuickCommentsSubTab`, `DrawAnnotateCommentsSubTab`, `DrawWhisperTestMicrophone`, `DrawWhisperTestE2E` | 8 | light (Ui/) | `funcsize_preferences_tabs` + ai-prefs bucket-E |
| **B — Views / Grid UI** | `drawViewsFieldsTab`, `drawViewsSortTab`, `drawViewsDashboardWindow`, `drawActiveProjectWindow`, `drawActiveProjectGridCell`, `DrawDraftIdColumnCell`, `drawAttachmentListPane`, `drawBulkExportWindow` | 8 | light | `funcsize_grid_render`, `views_columns_reorder` |
| **C — Shell / menus / render UI** | `drawMainMenuBar`, `drawMenuBarAppearanceMenu`, `drawSecondaryWindows`, `SmatchetProjectPicker::Draw`, `ApplyNortonCommander`, `SmatchetApplyImGuiFont`, `SmatchetImGuiHost::Initialize`, `SmatchetPerfUi::DrawWindow`, `CommandPaletteUi::Draw`, `TrackerQueryAcp_InputTextCallback`, `DrawAnnotatePersistedOptionsForm`, `DecodeImageFileToRgba32`, `RenderHistoryTurn`, `SmatchetDrawAiAssistantPanel`, `AdfEnterBlock`, `PreviewLeaveBlock`, `SelectableTextRun::End` | 17 | light | `funcsize_main_ui_smoke`, `funcsize_window_render_smoke` |
| **D — Tracker (STRICT)** | `FetchIssuesViaRestApi`, `MapIssueOrPullRequestJsonToCachedTicket`, `Tokenize` (JQL), `IssueTableSerializer::ParseJson`, `UpdateIssueFieldsViaTransition`, Jira `FetchIssuesStreamed`, `FetchIssuesForKeys`, Plane `FetchFieldCatalog`, Plane `FetchIssuesStreamed`, `RenderDateTimeFieldEditor`, `RenderGenericDatePicker`, `RenderLabelsFieldEditor` | 12 | **strict** | tracker ctest + bucket-A on extracted pure helpers |
| **E — Non-UI: Commands/Config/Sync/Mcp/App/misc (STRICT+)** | `RegisterConfigSetCommand`, `RegisterDebugGridEditBurstCommand`, `RegisterSyncCommands`, `CommandRegistry::Dispatch`, `SaveSecretsAndPurgeLegacy`, `RunLegacyProjectSweep`, `ReplayOneFieldEdit`, `McpPlugin::HandleToolsCall`, `HandleJsonRpcToolsCall`, `PrefetchIssueTicketsForKeys`, `ResolveFieldIconAssetPath`, `WarmIssueTypeEditMetaAtStartAsync`, `SubmitFieldEditNetworkOnly`, `TryRenderCachedLuaField`, `DrawLuaWindows`, `SubmitBugReport`, `InsertIntoFocusedInputText`, `SmatchetWhisperSetupBanner::Render`, `RenderTimeTrackingModal`, `RenderFieldCell`, `DrawDurationFieldWithSuggestions`, `ModelDownloader::RunDownloadWorker`, `StandaloneAppBootstrap::Initialize` | 23 | strict + light | ctest + dual-target build + relevant bucket-E |

Batch order A→B→C (UI, mechanical, well-covered) then D→E (strict, higher care). Each batch rebases on
latest `develop` before opening its PR; `function_size_audit.py --diff origin/develop` must show the
batch's functions gone from the WARN set with **no new** hard-cap entries.

## Files to modify

Per batch, the `.cpp` (+ its `.h` when a private helper API or `WindowState` member is added) for each
named function. New `*_detail.h` / `*Pure.cpp` seams only where extracting bucket-A-testable pure logic.
No edit to `docs/high-integrity/function-size-baseline.md` per batch (Rule 7); regen once at end.

## Existing utilities reused

- [`imgui-draw-pattern.md`](../../guides/imgui-draw-pattern.md) — the canonical UI decomposition recipe.
- `function_size_audit.py --scan-file <f>` / `--diff origin/develop` — per-function verification.
- `tests/ui/funcsize_*.test.cpp` — the bucket-E smoke harness already built for this work
  (`funcsize_preferences_tabs`, `funcsize_grid_render`, `funcsize_main_ui_smoke`, `funcsize_window_render_smoke`).
- `SMATCHET_UI_PERF_SCOPE` seams + `UiDrawSession` ctx carrier — section boundaries + per-frame ctx.
- Per-subsystem `*Pure` / `*_detail` seams (e.g. `SmatchetToolbarUi_detail.h`, `TrackerFieldCatalogPure`).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: zero-impact target. Extraction reuses existing perf scopes
  verbatim; no new per-frame allocation. Any batch touching a hot draw runs the matching scenario
  before/after — > 0.2 ms regression = revert.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: layout-only reorganisation; no new sync I/O. Existing
  async dispatch unchanged.
- **Pillar 3 (never crash)**: each batch dual-target build + (batches touching HTTP/bootstrap) ASan run.
  Positional-ImGui pairing preserved byte-for-byte.
- **Pillar 4 (accessibility)**: no change; hotkey dispatch + tab order preserved.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

Touches `Source/Core/` — **NOT N/A**.

1. **PR-fast CI** — each batch PR declares its scenario(s): Batch A → `preferences-window-render`;
   Batch B → `views-dashboard-render` + `active-project-window-render`; Batch C → `app-cold-start` +
   `main-ui-render`; Batch D → `bulk-payload-build-1000` / `jql-search`; Batch E → matching per-fn scenario.
2. **Pillar 2 static scanner** — no new sync I/O reachable from `ImGui::*` (extraction is layout-only).
3. **Dispatcher drain** — `MainThreadDispatcher::Drain()` not touched.
4. **Visible-cue bucket-E** — no new > 100 ms sync-stall path introduced.
5. **Marker inventory** — perf-scope names preserved verbatim; a batch that adds a scope regens
   `docs/perf/MARKER_INVENTORY.md` in that PR.

**Pre-push local check**: each batch runs its named scenario per `docs/guides/perf-workflow.md`.
**Override**: `perf-out-of-band` not anticipated — a batch needing it has gone wrong; halt + re-slice.

## Risks / non-goals

**Risks:**
- **Positional-ImGui regression** (UI batches) — Begin/End, PushID/PopID, BeginTable/EndTable pairing.
  → keep each pair inside one helper; bucket-E screenshot/interaction diff vs pre-refactor golden; any delta = revert.
- **Strict-zone lint hard-fail** (batches D/E) — Tracker/Config/Commands/Sync/Mcp fail on ANY violation.
  → run `test-lint-rules.sh --diff origin/develop` locally before every push; extracted helpers obey RAII/logging rules.
- **Extraction changes behaviour** — a mis-cut helper alters control flow. → behaviour-preserving only;
  expand ctest where a pure helper is carved; no logic rewrite.
- **Branch-count functions masquerading** — some over-100-line functions are ALSO over 20 branches; an
  extraction that only moves lines may leave the branch WARN. → acceptable (branch WARNs are out of scope);
  the line WARN is what this plan clears.
- **Campaign churn vs in-flight UI** — sequence batches to land fast; rebase each on develop; LuaConsole-adjacent
  work waits on open UI PRs (e.g. #881).

**Non-goals:**
- The 64 branch-only (>20 branches, ≤100 lines) functions — explicitly deferred (restructure risk).
- Any logic/behaviour change, new UI framework, or widget rewrite — pure mechanical decomposition.
- Editing the function-size baseline per batch — regen once at campaign end.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps where possible.

- **Per-function**: `function_size_audit.py --scan-file <f>` shows the function gone from the WARN set
  (under 100 lines); `--diff origin/develop` shows no NEW hard-cap entry.
- **Bucket A (ctest)**: each carved pure helper (non-UI batches D/E) gets a `tests/Core/*.test.cpp` case.
- **Bucket E (ImGui Test Engine)**: UI batches gated on the matching `funcsize_*` / feature test vs the
  pre-refactor golden. Windows with no coverage ship the refactor + a `docs/self-improvement/categories/test.md`
  gap entry naming the missing scenario (no silent residue).
- **Build gate**: every batch — `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Lint gate**: every batch — `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` (strict-zone batches especially).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` green (defer to the script).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model
  (sharpen "soft-warn vs hard-cap", "extract vs restructure") before finalising; record the outcome. Required — do not delete.
- **Manual residue**: none designed. Any window lacking bucket-E coverage → `categories/test.md` deferred-automation entry.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Scope-reduction edits: before finalising,
grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray refs to the
deferred branch-only set, and revise/delete.

- **The 64 branch-only (>20 branches) functions** — deferred; restructuring decision count is higher-risk
  logic surgery, not mechanical extraction. Tracked as residual non-blocking advisories; a future plan may take them.
- **Lowering the soft-warn threshold or making it block** — out of scope; the tier stays advisory.
- **Decomposing functions already ≤100 lines toward the 40-80 ideal** — opportunistic only, not a target.

## Implementation log

All five batches shipped + merged to `develop` (2026-06-06), one PR each, each dual-target build
green (`/WX`, zero warnings) + funcsize/comment/strict-lint gates clean, labelled `tests-out-of-band`:

- **Batch A — Preferences UI** (8 fns) · PR #892 · `SmatchetPreferencesUi{,_Local,_Templates,_Whisper}.cpp`.
  Collapsed a byte-identical Quick/Annotate-comments clone into one shared sub-tab helper (DRY/Pillar 5)
  and a 3× copy-paste inherit-field block into a templated `ApplyInheritFieldsBuf`.
- **Batch B — Views/Grid UI** (9 fns incl. an Attachment ride-along) · PR #894 ·
  `SmatchetActiveProjectGridUi.cpp`, `SmatchetViewsDashboardUi.cpp`, `SmatchetNewIssueDraftUi.cpp`,
  `SmatchetAttachmentPreviewUi.cpp`, `SmatchetBulkTicketsUi.cpp` (+ `SmatchetUI.h` helper decls).
- **Batch C — shell/menus/render UI** (~17 fns) · PR #896 · main-menu / shell / project-picker / theme /
  fonts / host / perf-UI / command-palette (strict) / autocomplete / annotate-prefs / AI-assistant /
  markdown / selectable-text (+ 4 headers).
- **Batch D — Tracker (strict zone)** (11 fns) · PR #897 · GitHub/Jira/Plane fetch+mapping+mutation,
  JQL tokenize, table-serialize, date/labels editors. Non-UI logic extracted as unit-testable pure helpers.
- **Batch E — non-UI Commands/Config/Sync/Mcp/App** (24 fns) · PR #898 · command registrars,
  config-secrets, offline-queue, MCP handlers, AppController workers, bug-report, dictation, whisper
  banner, ticket-field editors, model-downloader, standalone bootstrap.

Prereq: PR #889 (archived `decompose-top-20-monoliths` + regenerated the stale hard-cap baseline 116→0).

## Deviations from plan

- **Batches C/D/E delegated to subagents** (the plan implied inline work). Each followed a fixed
  decomposition playbook; the orchestrator re-verified every dual-target build independently — which
  caught a `C4100` (unused `State&` param in `DrawRecentSection`) the Batch C subagent's still-compiling
  build had missed. Standing lesson: always re-verify a delegated agent's build.
- **No end-of-campaign baseline regen needed.** The plan said "regen the baseline once at campaign end",
  but `function-size-baseline.md` only tracks **hard-cap** functions (already 0 after #889); the soft-tier
  functions this campaign decomposed were never in it, so it is unchanged. Dropped as a no-op.
- **Attachment `DecodeImageFileToRgba32` decomposed in Batch B** (not C) — it shares
  `SmatchetAttachmentPreviewUi.cpp` with `drawAttachmentListPane`, so both were done together to keep the
  file in a single PR. One-file-one-PR discipline over batch labels.
- **Two pre-existing best-effort `catch(...)` fallbacks annotated** with `// catch-all-ok` (Jira file-size
  estimate, Plane project-name lookup, config-set JSON-parse) — surfaced by the whole-file lint drain on
  touched strict-zone files; pre-existing on develop, annotated for cleanliness.

## Verification (actual)

- **Tree-wide result: 0 functions over 100 lines** (`function_size_audit.py --scan-file` across all
  first-party `Source/**/*.cpp`) — down from 68 at campaign start. Target fully met.
- **Per batch**: dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`, `/WX`) green, zero
  warnings; `function_size_audit.py --diff origin/develop` exit 0; `comment_audit.py --diff` clean;
  `test-lint-rules.sh --diff` PASS (strict zones Commands/Config/Sync/Mcp/Tracker included).
- **CI**: every required check green on each PR (one Bucket-C run hit a Mesa-cache infra flake — 3-second
  death before any build/render — cleared on re-run, not a visual regression). The #894↔#896 shared
  `SmatchetUI.h` 3-way merge was verified by a local dual-target build before #896 merged.
- **Out-of-scope, confirmed deferred**: the 64 branch-only (>20 branches, ≤100 lines) functions — they
  need logic restructuring, not extraction; tracked as residual non-blocking advisories.
- **Known non-blocking follow-up**: Batch E's extracted `StandaloneAppBootstrap` helpers textually match
  `main.cpp`'s parallel bootstrap blocks → a `[dup] WARN` (pre-existing duplication surfaced; WARN-first
  under ADR-0015). A shared standalone-bootstrap helper used by both would clear it.

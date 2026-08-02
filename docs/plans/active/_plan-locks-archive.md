# Plan-locks — frozen historical archive

> **🛑 FROZEN at Phase 6 cutover (2026-05-17).**
>
> This file is no longer the live coordination surface. New plan-lock claims
> live in `refs/locks/<slug>` per the [`git-ref-plan-locks`](../shipped/git-ref-plan-locks.md) design.
>
> - **Live coordination state**: `bash agents/scripts/core/locks-show.sh` (canonical) or
>   [`_plan-locks.generated.md`](./_plan-locks.generated.md) (snapshot, may lag up to 30 min).
> - **Claim a new lock**: `bash agents/scripts/core/lock-claim.sh <slug> <write-set-file>`.
> - **Update scope mid-slice**: `bash agents/scripts/core/lock-claim-update.sh <slug> <write-set-file>`.
> - **Release a lock (manual)**: `bash agents/scripts/core/lock-release.sh <slug>` — usually auto-handled by
>   [`.github/workflows/lock-cleanup.yml`](../../../.github/workflows/lock-cleanup.yml) on PR-merge.
> - **Plan + rationale**: [`docs/plans/shipped/git-ref-plan-locks.md`](../shipped/git-ref-plan-locks.md).
>
> Hand-edits below the Protocol section are **no longer authoritative** and may be removed
> in future archive cleanups. The Protocol section is retained for reference on the
> entry-shape grammar still used by `claim.json`.

This file preserves the audit trail of every plan-lock that existed between project inception and the 2026-05-17 cutover. Entries below are historical only.

## Protocol

Every plan that ships in more than one PR (or that hands off to delegated agents) appends an entry here **before** the first edit and updates it on every state transition.

**Entry shape:**

```
### <plan-slug> · <slice-id> · status: <claimed|in-flight|shipped|on-hold|abandoned>

- **Branch**: `claude/<branch-name>` (or `feat/<branch-name>` for autonomous-plan branches)
- **Owner agent**: `<agent-name>` (or `orchestrator` if direct)
- **Originating plan**: [`docs/plans/active/<plan>.md`](./<plan>.md) § <section>
- **Claimed write set** (paths the slice will edit / create / delete):
  - `<path 1>`
  - `<path 2>`
  - ...
- **Read-only adjacency** (paths the slice reads but does not edit — list when high overlap risk):
  - `<path>`
- **Started**: `<YYYY-MM-DD>`
- **Last update**: `<YYYY-MM-DD>` — `<one-line state change>`
- **Cleared by**: PR `#<number>` merged at `<sha>` (fill on `shipped` / `abandoned`).
```

**Pre-flight check — every orchestrator + every delegation packet:**

1. Read this file.
2. Compute the intersection of the new slice's planned write set with every `status: claimed | in-flight` entry below.
3. **Empty intersection** → append the new claim, proceed.
4. **Non-empty intersection** → STOP. Either:
   - Coordinate with the holding slice — adjust scope, sequence behind it, or pick a different slice.
   - Promote the conflict to the user via `AskUserQuestion` with the overlap inventoried.

Agent prompts must include the lock-file path explicitly. The standard wording added to `AGENTS.md` § Orchestrator delegation packet is: *"Read `docs/plans/active/_plan-locks.md` first. Refuse if your write set overlaps an `in-flight` or `claimed` entry; surface to the orchestrator."*

**State transitions:**

- `claimed` — orchestrator added the entry, no commits yet on the branch.
- `in-flight` — at least one commit pushed; PR may or may not be open.
- `shipped` — PR merged; entry stays for ~one merge window then prunes.
- `on-hold` — entry retained without active work; downstream slices may pre-emptively claim.
- `abandoned` — branch dropped without merge; entry pruned immediately.

**Pruning:** `shipped` entries that are older than 14 days OR whose merge sha is already in `git log origin/develop` should be deleted in the next coordination PR. Keep this file shallow.

## Shipped + abandoned archive

_Originally the in-flight section. The lone `git-ref-plan-locks` entry that lived here was migrated to `refs/locks/git-ref-plan-locks` at Phase 6 cutover (2026-05-17). The historical shipped + abandoned entries below remain for audit-trail reference._

### h12-l16-m13-bundle · slice-1 · status: shipped (PR #196 merged at 1952e8b)

- **Branch**: `feat/h12-l16-m13-bundle`
- **Owner agent**: `claude` (orchestrator-dispatched general-purpose)
- **Originating plan**: [`docs/plans/shipped/pillar-1-2-audit-2026-05-17.md`](../shipped/pillar-1-2-audit-2026-05-17.md) § Open / backlog — H12 + L16 + M13
- **Claimed write set**:
  - `Source_Core/src/SmatchetBulkTicketsUi.cpp` (H12 — Load file + Save to file buttons)
  - `Source_Core/src/SmatchetFieldIconRender.cpp` (L16 — URL-disk-cache-hit branch + file-path branch)
  - `Source_Core/src/TicketFieldEditor.cpp` (M13 — OpenLongTextEditor threshold-gated worker dispatch)
  - `Source_Core/include/SmatchetUiSession.h` (transient in-flight state for H12 bulk load/save)
  - `docs/plans/active/_plan-locks.md` (this entry)
  - `docs/plans/shipped/pillar-1-2-audit-2026-05-17.md` (Shipped + Watch list section)
- **Read-only adjacency**: `Source_Core/src/SmatchetGridFieldEditPipeline.cpp` (PR #186 pattern reference), `Source_Core/include/MainThreadDispatcher.h`, `Source_Core/include/AppController.h`
- **Started**: 2026-05-17
- **Last update**: 2026-05-17 — merged via PR [#196](https://github.com/alexandrosk0/Smatchet/pull/196) at sha `1952e8b`. Pure-helper `TicketFieldEditorLongTextPure` TU added in follow-up commit `d7da177` to satisfy the test-delta gate.
- **Cleared by**: PR `#196` merged at `1952e8b`.

### configmanager-save-coalesce · slice-1 · status: shipped (PR #190 merged at a3298ca)

- **Branch**: `feat/configmanager-save-coalesce` (deleted)
- **Owner agent**: `claude` (orchestrator-dispatched)
- **Originating plan**: [`docs/plans/shipped/pillar-1-2-audit-2026-05-17.md`](../shipped/pillar-1-2-audit-2026-05-17.md) § H11 + § Pillar 1 P1
- **Claimed write set**:
  - `Source_Core/include/SmatchetUiSession.h` (MOD — add `prefsDirty` + `prefsSaveDueAt` + `MarkPrefsDirty` helper)
  - `Source_Core/src/SmatchetPreferencesUi.cpp` (MOD — 31 sites replaced with `MarkPrefsDirty(d)`; 3 AI Assistant tab sites preserved at lines 953/1024/1048)
  - `Source_Core/src/SmatchetUI.cpp` (MOD — end-of-frame debounced fire at Draw tail)
  - `Source_Core/src/SmatchetUI_Layout.cpp` (MOD — final sync Save in `DrainUiDrawSessionFuturesBeforeAppTeardown`)
  - `docs/plans/active/_plan-locks.md`
- **Read-only adjacency**: `Source_Core/include/ConfigManager.h`, `Source_Core/src/ConfigManager.cpp`
- **Started**: 2026-05-17
- **Last update**: 2026-05-17 — merged via PR [#190](https://github.com/alexandrosk0/Smatchet/pull/190) at sha `a3298ca`. 100 ms debounce; AI Assistant tab Save flow preserved.
- **Cleared by**: PR `#190` merged at `a3298ca`.

### pillar-2-top5-fixes · slice-1 · status: shipped (PR #191 merged at 8b779bc)

- **Branch**: `feat/pillar-2-top5-fixes`
- **Owner agent**: `claude` (orchestrator-dispatched)
- **Originating plan**: [`docs/plans/shipped/pillar-1-2-audit-2026-05-17.md`](../shipped/pillar-1-2-audit-2026-05-17.md) § Pillar 2 — CRITICAL (findings 1-9)
- **Claimed write set**:
  - `Source_Core/src/SmatchetFieldIconRender.cpp` (finding #1 — icon fetch worker dispatch + loading sentinel)
  - `Source_Core/src/SmatchetAttachmentPreviewUi.cpp` (finding #2 — attachment download worker dispatch)
  - `Source_Core/src/SmatchetAutocompleteUi.cpp` (finding #3 — JQL @-mention via std::async)
  - `Source_Core/src/SmatchetGridUiSupport.cpp` (finding #4 — quick-comment worker dispatch)
  - `Source_Core/src/BlameAnalysisUi_Modals.cpp` (finding #5/#6 — blame profile + assign-prepare worker)
  - `Source_Core/src/BlameAnalysisUi_Window.cpp` (finding #7 — blame assign-commit worker chain)
  - `Source_Core/src/SmatchetUI.cpp` (finding #8 — installer download worker + progress modal, DrawAppUpdateModal only)
  - `Source_Core/include/SmatchetUiSession.h` (transient in-flight state for installer + comment + blame ops)
  - `Source_Core/include/AppController.h` + `Source_Core/src/AppController.cpp` (MOD — `DownloadAndLaunchInstallerUpdate` gains optional cancel atom)
  - `Source_Core/src/BlameAnalysisUi_Internal.h` (BlameState gates)
  - `docs/plans/active/_plan-locks.md` (this entry)
- **Read-only adjacency**: `Source_Core/src/SmatchetGridFieldEditPipeline.cpp` (PR #186 pattern), `Source_Core/include/MainThreadDispatcher.h`
- **Started**: 2026-05-17
- **Last update**: 2026-05-17 — merged via PR [#191](https://github.com/alexandrosk0/Smatchet/pull/191) at sha `8b779bc`. All 9 sync HTTP/IO sites moved to worker threads via `LaunchBackgroundTask` + `MainThreadDispatcher`.
- **Cleared by**: PR `#191` merged at `8b779bc`.

### grid-cell-edit-perf · slice-1 · status: shipped (PR #186 merged at 16e0611)

- **Branch**: `feat/grid-cell-edit-perf`
- **Owner agent**: `claude` (orchestrator-dispatched general-purpose; perf-detective workflow inline since user is offline)
- **Originating plan**: orchestrator-direct (user report: "When I edit any value, there is a pause...")
- **Claimed write set**:
  - `Source_Core/src/SmatchetGridFieldEditPipeline.cpp` (MOD — replace inline `SubmitFieldEditNetworkOnly` call with worker dispatch + main-thread post-back)
  - `Source_Core/include/SmatchetUiSession.h` (MOD — add worker-dispatch tracking flags + result-staging fields on `UiDrawSession`)
  - `Source_Core/src/Commands/Builtin/BuiltinCommands_Perf.cpp` (MOD — add `debug.grid.edit-burst` command)
  - `scripts/dev/test-grid-edit-perf-postfix.sh` (NEW — regression gate, auto-enrolled by `test-all.sh`)
  - `scripts/dev/test-grid-edit-perf-baseline.sh` (NEW — baseline capture; `test-` prefix but lenient — does not assert)
  - `scripts/dev/manual-grid-edit-perf-compare.sh` (NEW — `manual-` prefix means NOT auto-enrolled)
  - `docs/plans/shipped/grid-cell-edit-perf.md` (NEW — plan + implementation log)
  - `docs/plans/active/_plan-locks.md` (this entry)
- **Read-only adjacency**: `Source_Core/src/AppController_CatalogAndFieldEdit.cpp`, `Source_Core/include/MainThreadDispatcher.h`, `Source_Core/include/AppController.h`
- **Started**: 2026-05-17
- **Last update**: 2026-05-17 — claimed.
- **Cleared by**: TBD PR.

### ai-assistant-fixes-batch-2 · slice-1 · status: shipped (PR #184 merged at f61315e)

- **Branch**: `feat/ai-assistant-fixes-batch-2`
- **Owner agent**: `claude` (orchestrator-dispatched general-purpose)
- **Originating plan**: orchestrator-direct (3 fixes; follow-up to PR #181 user testing)
- **Claimed write set**:
  - `Source_Core/include/ConfigManager.h` (MOD — 1 new TrackerConfig field `AiPrefsVerifyOnSave`)
  - `Source_Core/src/ConfigManager.cpp` (MOD — serialize + `j.value()` default)
  - `Source_Core/include/SmatchetUiSession.h` (MOD — transient probe state)
  - `Source_Core/src/SmatchetPreferencesUi.cpp` (MOD — explicit Save/Discard/Test-connection flow)
  - `docs/plans/active/_plan-locks.md` (this entry)
- **Read-only adjacency**: `Source_Core/include/IAiClient.h`, `Source_Core/include/AiClientFactory.h`, `Source_Core/include/MainThreadDispatcher.h`, `Source_Core/include/AiPrefsValidator.h`
- **Started**: 2026-05-17
- **Last update**: 2026-05-17 — claimed.
- **Cleared by**: TBD PR.

### ai-assistant-fixes-batch-1 · slice-1 · status: shipped (PR #181 merged at a3c7411)

- **Branch**: `feat/ai-assistant-fixes-batch-1`
- **Owner agent**: `claude` (orchestrator-dispatched general-purpose)
- **Originating plan**: orchestrator-direct (5 user-reported fixes; no design doc)
- **Claimed write set**:
  - `Source_Core/include/ConfigManager.h` (MOD — 2 new TrackerConfig fields)
  - `Source_Core/src/ConfigManager.cpp` (MOD — serialize + j.value() defaults)
  - `Source_Core/src/SmatchetAiAssistantUi.cpp` (MOD — Fix 1 dock + Fix 2 Enter)
  - `Source_Core/include/SmatchetAiAssistantUi.h` (MOD — if needed)
  - `Source_Core/src/SmatchetUI.cpp` (MOD — Fix 1 repairTopLevelWindow tweak)
  - `Source_Core/include/SmatchetUiSession.h` (MOD — drop assistantPanelWidthLive)
  - `Source_Core/include/AgentsMdLoader.h` (MOD — Fix 3 opt-in flag)
  - `Source_Core/src/AgentsMdLoader.cpp` (MOD — Fix 3)
  - `Source_Core/src/AiContextBuilder.cpp` (MOD — Fix 3 call-site)
  - `Source_Core/src/AiClientFactory.cpp` (MOD — Fix 4 relabel)
  - `Source_Core/include/AiPrefsValidator.h` (MOD — Fix 4 + Fix 5b)
  - `Source_Core/src/AiPrefsValidator.cpp` (MOD — Fix 4 + Fix 5b)
  - `Source_Core/src/SmatchetPreferencesUi.cpp` (MOD — Fix 3 + Fix 4 + Fix 5)
  - `tests/Source_Core/AgentsMdLoader.test.cpp` (MOD — Fix 3)
  - `tests/Source_Core/AiPrefsValidator.test.cpp` (MOD — Fix 4 + Fix 5b)
  - `scripts/dev/manual-ai-lmstudio-send.sh` (NEW — Fix 4)
  - `docs/plans/active/_plan-locks.md` (this entry)
- **Read-only adjacency**: `Source_Core/include/SmatchetDockNodeIds.h`, `Source_Core/include/Views.h`, `Source_Core/include/AppController.h`, `Source_Core/include/AiAssistantController.h`.
- **Started**: 2026-05-17
- **Last update**: 2026-05-17 — claimed.
- **Cleared by**: TBD PR.

### ai-assistant-side-panel · Hotfix-batch-2 · p0-p1-retrospective-sweep · status: shipped (PR #176 merged at 2ef403e)

- **Branch**: `fix/ai-feature-p0-p1-hotfix` (deleted)
- **Owner agent**: `orchestrator`
- **Originating plan**: orchestrator-direct (retrospective review of [`docs/plans/shipped/ai-assistant-side-panel.md`](../shipped/ai-assistant-side-panel.md) by `code-review` + `security-review` after merge of Phase E; 4 CRITICAL + 8 HIGH findings)
- **Claimed write set**:
  - `Source_Core/src/AnthropicClient.cpp` (MOD — redact error body)
  - `Source_Core/src/OllamaClient.cpp` (MOD — redact error body)
  - `Source_Core/src/AiErrorRedact.cpp` (MOD — add `x-api-key` JSON-field rule)
  - `Source_Core/src/AiAssistantController.cpp` (MOD — URL allow-list, luaContext_ mutex, cancel-atom race, agents.md cache)
  - `Source_Core/include/AiAssistantController.h` (MOD — mutex member, cache members)
  - `Source_Core/src/SmatchetAiAssistantUi.cpp` (MOD — defer ConfigManager::Save, deferred audit-trail fetch, static-buf re-seed)
  - `Source_Core/src/AiSseParser.cpp` (MOD — single-space strip per RFC + buffer cap)
  - `Source_Core/include/AiSseParser.h` (MOD — buffer cap constant)
  - `Source_Core/src/AiNdjsonParser.cpp` (MOD — buffer cap)
  - `Source_Core/include/AiNdjsonParser.h` (MOD — buffer cap constant)
  - `Source_Core/src/AiContextBuilder.cpp` (MOD — split audit-trail body so caller can defer the fs read)
  - `Source_Core/include/AiContextBuilder.h` (MOD — surface deferred-audit hook)
  - `Source_Core/src/AppController.cpp` (MOD — no lazy ctor post-shutdown)
- **Read-only adjacency**: `Source_Core/include/AppController.h`, `Source_Core/include/SmatchetUiSession.h`, `Source_Core/include/ConfigManager.h`, `Source_Core/include/BackendAuditTrail.h`.
- **Started**: 2026-05-17
- **Last update**: 2026-05-17 — merged via PR [#176](https://github.com/alexandrosk0/Smatchet/pull/176) at sha `2ef403e`.
- **Cleared by**: PR `#176` merged at `2ef403e`.

### ai-debug-cli-and-prefs-validation · slice-1 · status: shipped (PR #174 merged at 8ba3dc3)

- **Branch**: `feat/ai-debug-cli-and-prefs-validation` (deleted)
- **Owner agent**: `claude` (orchestrator-dispatched general-purpose)
- **Originating plan**: orchestrator-direct (no design doc — investigation + tooling slice)
- **Claimed write set**:
  - `Source_Core/include/AiModelCatalog.h` (NEW)
  - `Source_Core/src/AiModelCatalog.cpp` (NEW)
  - `Source_Core/include/AiPrefsValidator.h` (NEW)
  - `Source_Core/src/AiPrefsValidator.cpp` (NEW)
  - `Source_Core/src/AnthropicClient.cpp` (MOD — LOG_ERROR on non-2xx)
  - `Source_Core/src/OpenAiClient.cpp` (MOD — LOG_ERROR on non-2xx)
  - `Source_Core/src/Commands/Builtin/BuiltinCommands_Ai.cpp` (NEW — 5 ai.* commands)
  - `Source_Core/src/Commands/Builtin/BuiltinCommands_Internal.h` (MOD — declare RegisterAiCommands)
  - `Source_Core/src/Commands/BuiltinCommands.cpp` (MOD — call RegisterAiCommands)
  - `Source_Core/src/SmatchetPreferencesUi.cpp` (MOD — validator banners + model dropdown)
  - `tests/Source_Core/AiModelCatalog.test.cpp` (NEW)
  - `tests/Source_Core/AiPrefsValidator.test.cpp` (NEW)
  - `tests/CMakeLists.txt` (MOD — register 2 tests + 2 cpps)
  - `scripts/dev/test-ai-prefs-validator.sh` (NEW — auto-enrolled)
  - `scripts/dev/manual-ai-anthropic-probe.sh` (NEW — opt-out by name)
  - `scripts/dev/manual-ai-anthropic-send.sh` (NEW — opt-out by name)
  - `docs/plans/active/_plan-locks.md` (this entry)
- **Read-only adjacency**: `Source_Core/include/IAiClient.h`, `Source_Core/include/AiTypes.h`, `Source_Core/include/AiClientFactory.h`, `Source_Core/include/AiSseParser.h`, `Source_Core/src/AiClientFactory.cpp`, `Source_Core/include/ConfigManager.h`
- **Started**: 2026-05-17
- **Last update**: 2026-05-17 — merged via PR [#174](https://github.com/alexandrosk0/Smatchet/pull/174) at sha `8ba3dc3`. HTTP-error logging in both AI clients; `AiModelCatalog` + `AiPrefsValidator` doctest-covered (14 + 6 cases, 1 `[high-risk]`); 5 CLI commands (`ai.{list-models,dump-request,probe,send-once,validate-prefs}`); Preferences UI model Combo + live validation banners (Test-connection async button deferred — existing tab uses per-field autosave). Test aggregate 388 cases / 1994 assertions (+17/+100). `claude-sonnet-4-6` confirmed valid Anthropic ID — 400 cause is at the wire boundary (expired key / tier / quota), now diagnosable via `ai.send-once`'s `response_body_excerpt` + new `LOG_ERROR`.
- **Cleared by**: PR `#174` merged at `8ba3dc3`.

### ai-assistant-side-panel · Phase E · lua-glue-schema-bump-and-docs · status: shipped (PR #170 merged at f2d0933)

- **Branch**: `feat/ai-assistant-side-panel-phase-e` (deleted)
- **Owner agent**: `lua-binder`
- **Originating plan**: [`docs/plans/shipped/ai-assistant-side-panel.md`](../shipped/ai-assistant-side-panel.md) § File-level changes (Phase E rows) — Lua glue + stubs + LayoutSchemaVersion bump + README/LUA_GUIDE bullets
- **Claimed write set**:
  - `Source_Core/src/AppController_LuaBindings.cpp` (MOD — restore 3 `ai.*` glues registered on `state["ai"]`)
  - `Source_Core/src/AppController_LuaStubs.cpp` (MOD — no-op stub parity for `ai.*` Lua-callable names so script-load works under `SMATCHET_WITH_LUA_AUTOMATION=0`)
  - `Source_Core/include/ConfigManager.h` (MOD — `kCurrentLayoutSchemaVersion` 5→6, single bump for the whole feature)
  - `README.md` (MOD — one feature bullet)
  - `docs/guides/lua.md` (MOD — one `ai.*` bullet + short example)
  - `docs/self-improvement/categories/process.md` (MOD — latest-first P2 worktree-bootstrap entry)
  - `docs/plans/active/_plan-locks.md` (this entry + Phase D flip to shipped)
  - `docs/plans/shipped/ai-assistant-side-panel.md` (Implementation log + Deviations + Verification for Phase E — closes plan)
- **Read-only adjacency**: `Source_Core/include/AppController.h` (always-on `AddAiContext` / `ClearAiContext` / `PromptAi` shipped Phase B), `Source_Core/include/ILuaBindingHost.h`, `Source_Core/src/AppController_LuaBindingsCore.cpp` (Phase 6b InitLuaCore pattern + `__smatchet_app_ui` dual-key)
- **Started**: 2026-05-17
- **Last update**: 2026-05-17 — merged via PR [#170](https://github.com/alexandrosk0/Smatchet/pull/170) at sha `f2d0933`. 3 Lua glues registered on `state["ai"]` (resolve via `__smatchet_app_ui` per Phase 6b dual-key); `LayoutSchemaVersion` 5→6 (single bump for the whole feature per AGENTS.md § Schema-version bumps); README + LUA_GUIDE bullets; no LuaStubs.cpp parity needed (receivers are always-on AppController members from Phase B). Final phase — ai-assistant-side-panel plan closes here.
- **Cleared by**: PR `#170` merged at `f2d0933`.

### ai-assistant-side-panel · Phase D · clients-and-provider-combo · status: shipped (PR #169 merged at 1b45505)

- **Branch**: `feat/ai-assistant-side-panel-phase-d`
- **Owner agent**: `claude` (orchestrator-direct implementer, per plan)
- **Originating plan**: [`docs/plans/shipped/ai-assistant-side-panel.md`](../shipped/ai-assistant-side-panel.md) § Streaming protocol (Anthropic SSE + Ollama native) + § File-level changes (Phase D rows) + § Preferences extension
- **Claimed write set**:
  - `Source_Core/include/AiNdjsonParser.h` (NEW — line-buffered NDJSON sibling to `AiSseParser`)
  - `Source_Core/src/AiNdjsonParser.cpp` (NEW)
  - `Source_Core/include/AnthropicClient.h` (NEW — `IAiClient` impl driving `/v1/messages` Native Messages API)
  - `Source_Core/src/AnthropicClient.cpp` (NEW)
  - `Source_Core/include/OllamaClient.h` (NEW — `IAiClient` impl driving `/api/chat` NDJSON)
  - `Source_Core/src/OllamaClient.cpp` (NEW)
  - `Source_Core/src/AiClientFactory.cpp` (MOD — Anthropic + OllamaNative branches return non-null impls; drop Phase A placeholder LOG_WARN/nullptr)
  - `tests/Source_Core/AiNdjsonParser.test.cpp` (NEW — ≥ 6 cases / ≥ 30 assertions, ≥ 1 `[high-risk]`)
  - `tests/Source_Core/AiClientFactory.test.cpp` (MOD — flip 2 nullptr assertions to non-null + `GetProviderName()` match)
  - `Source_Core/src/SmatchetPreferencesUi.cpp` (MOD — complete Assistant group: provider Combo, masked OpenAI + Anthropic API key inputs, per-provider model inputs, base URL inputs)
  - `tests/CMakeLists.txt` (MOD — append `AiNdjsonParser.test.cpp` + `AiNdjsonParser.cpp` + `AnthropicClient.cpp` + `OllamaClient.cpp`)
  - `docs/plans/shipped/ai-assistant-side-panel.md` (revise — append Implementation log row + Deviations + extend Verification)
  - `docs/plans/active/_plan-locks.md` (this entry + Phase C flip to shipped)
- **Read-only adjacency**: `Source_Core/include/AiSseParser.h`, `Source_Core/src/AiSseParser.cpp`, `Source_Core/include/IAiClient.h`, `Source_Core/include/AiTypes.h`, `Source_Core/include/AiClientFactory.h`, `Source_Core/include/ConfigManager.h` (Ai field set already shipped Phase A'), `Source_Core/src/OpenAiClient.cpp` (template for cancel/WriteCallback/NetworkUsageTracker shape).
- **Started**: 2026-05-17
- **Last update**: 2026-05-17 — dispatched.
- **Cleared by**: TBD PR.

### ai-assistant-side-panel · Phase C · agents-md-loader-and-context-builder · status: shipped (PR #168 merged at 339eb24)

- **Branch**: `feat/ai-assistant-side-panel-phase-c` (deleted)
- **Owner agent**: `claude` (orchestrator-direct implementer, per plan)
- **Originating plan**: [`docs/plans/shipped/ai-assistant-side-panel.md`](../shipped/ai-assistant-side-panel.md) § agents.md loader + § Auto-context blocks + § File-level changes (Phase C rows)
- **Claimed write set**:
  - `Source_Core/include/AgentsMdLoader.h` (NEW)
  - `Source_Core/src/AgentsMdLoader.cpp` (NEW)
  - `Source_Core/include/AiContextBuilder.h` (NEW)
  - `Source_Core/src/AiContextBuilder.cpp` (NEW)
  - `Source_Core/src/SmatchetAiAssistantUi.cpp` (MOD — per-block context checkboxes near input area; build context via AiContextBuilder on Send)
  - `Source_Core/src/SmatchetPreferencesUi.cpp` (MOD — Assistant tab with agents.md path inputs only; provider/model/API key inputs deferred to Phase D)
  - `Source_Core/src/AiAssistantController.cpp` (MOD — system-prompt assembly uses AgentsMdLoader + builder output; UI-thread Submit)
  - `tests/Source_Core/AgentsMdLoader.test.cpp` (NEW)
  - `tests/Source_Core/AiContextBuilder.test.cpp` (NEW)
  - `tests/CMakeLists.txt` (MOD — register 2 new test files + 2 new production .cpps)
  - `docs/plans/shipped/ai-assistant-side-panel.md` (revise — append Implementation log row + Deviations + extend Verification)
  - `docs/plans/active/_plan-locks.md` (this entry)
- **Read-only adjacency**: `Source_Core/include/AiAssistantController.h`, `Source_Core/include/AiTypes.h`, `Source_Core/include/ConfigManager.h` (Ai field set + 5 context-block toggles + agents.md paths already shipped Phase A'), `Source_Core/include/AppController.h` (`GetActiveTicketsSnapshot` + `IsOnUiThread`), `Source_Core/include/SpreadsheetState.h` (`RectSel.Rows` + `ActiveIssueId`), `Source_Core/include/SmatchetUiSession.h` (`cachedSortedIndices` + `assistantContextBlock*` runtime mirror), `Source_Core/include/BackendAuditTrail.h` (`ReadRecentEvents`), `Source_Core/include/Views.h` + `ConfigManager::ViewDefinition` (active view + JQL + columns).
- **Started**: 2026-05-17
- **Last update**: 2026-05-17 — merged via PR [#168](https://github.com/alexandrosk0/Smatchet/pull/168) at sha `339eb24`. SmatchetTests aggregate post-Phase-C: 357 cases / 1836 assertions.
- **Cleared by**: PR `#168` merged at `339eb24`.

### ai-assistant-side-panel · Phase B · side-panel-ui-and-controller · status: shipped (PR #163 merged at dd703ab)

- **Branch**: `feat/ai-assistant-side-panel-phase-b` (deleted)
- **Owner agent**: `claude` (orchestrator-direct implementer, per plan)
- **Originating plan**: [`docs/plans/shipped/ai-assistant-side-panel.md`](../shipped/ai-assistant-side-panel.md) § File-level changes (Phase B rows) + § Side-panel layout + § Streaming protocol § Main-thread posting + § Cancellation
- **Claimed write set**:
  - `Source_Core/include/AiAssistantController.h` (NEW)
  - `Source_Core/src/AiAssistantController.cpp` (NEW)
  - `Source_Core/include/SmatchetAiAssistantUi.h` (NEW)
  - `Source_Core/src/SmatchetAiAssistantUi.cpp` (NEW)
  - `Source_Core/include/AppController.h` (MOD — `aiAssistant_` member + accessor + always-on stubs)
  - `Source_Core/src/AppController.cpp` (MOD — ctor wiring + dtor reset-at-top + stub bodies + AiAssistantController + AiTypes includes)
  - `Source_Core/include/SmatchetUI.h` (MOD — private `drawAiAssistantPanel` method decl)
  - `Source_Core/src/SmatchetUI.cpp` (MOD — include + Draw call site + Ctrl+Shift+A keybinding + member-impl delegating to free function)
  - `Source_Core/src/SmatchetUI_MainMenu.cpp` (MOD — View menu "Assistant (Ctrl+Shift+A)" toggle)
  - `Source_Core/src/SmatchetUI_Layout.cpp` (MOD — `repairTopLevelWindow` early-return on `layoutKey == "assistant_panel"`)
  - `Source_Core/include/SmatchetUiSession.h` (MOD — 10 `assistant*` fields gated `#if defined(SMATCHET_WITH_AI)`)
  - `CMakeLists.txt` (MOD — link `SmatchetCoreAiShim` PUBLIC to standalone-OpenGL core targets only)
  - `docs/plans/shipped/ai-assistant-side-panel.md` (revise — append Implementation log row + Deviations + extend Verification)
  - `docs/plans/active/_plan-locks.md` (this entry)
- **Read-only adjacency**: `Source_Core/include/IAiClient.h`, `Source_Core/include/AiTypes.h`, `Source_Core/include/AiClientFactory.h`, `Source_Core/include/AiSseParser.h`, `Source_Core/src/OpenAiClient.cpp`, `Source_Core/include/ConfigManager.h` (`AssistantPanelOpen` + `AssistantPanelWidth` + `AiProviderKind` + `AiApiKey` already shipped Phase A')
- **Started**: 2026-05-17
- **Last update**: 2026-05-17 — merged via PR [#163](https://github.com/alexandrosk0/Smatchet/pull/163) at sha `dd703ab`. 4 new files (`AiAssistantController.{h,cpp}` worker controller + `SmatchetAiAssistantUi.{h,cpp}` right-anchored panel); 8 existing files modified (`AppController` member + ctor/dtor + always-on stubs; `SmatchetUI` Draw call + Ctrl+Shift+A keybinding; `SmatchetUI_MainMenu` View entry; `SmatchetUI_Layout` repair early-return; `SmatchetUiSession.h` 10 fields). `SMATCHET_WITH_AI` shim now linked to standalone-OpenGL core targets. ON + OFF builds both green. Threading invariants encoded: per-turn cancel atom, MainThreadDispatcher worker→UI hand-off, stale-callback drop via `assistantTurnGen`, dtor ordering `aiAssistant_.reset()` before `mainThreadDispatcher.BeginShutdown()`. Phase C (agents.md loader + context builder) unblocked.
- **Cleared by**: PR `#163` merged at `dd703ab`.

### lua-host-friend-drop · slice-1 · status: shipped (PR #151 merged at 53b2881)

- **Branch**: `feat/lua-host-friend-drop` (deleted)
- **Owner agent**: `lua-binder`
- **Originating plan**: [`docs/plans/shipped/lua-host-friend-drop.md`](../shipped/lua-host-friend-drop.md)
- **Claimed write set**:
  - `Source_Core/include/AppController.h` (drop `friend class LuaAutomationHost;` + surrounding comment block at lines 105-109; drop `class LuaAutomationHost;` forward-decl if no longer needed)
  - `Source_Core/include/LuaAutomationHost.h` (default ctor; remove `AppController& app_;` field + forward-decl; rewrite header doc-comment)
  - `Source_Core/src/LuaAutomationHost.cpp` (drop `#include "AppController.h"`; simplify ctor body)
  - `Source_Core/src/AppController.cpp` (line 1062 — `make_unique<LuaAutomationHost>()` with no `*this` arg)
  - `docs/plans/shipped/large-files-and-phase-2.md` (B1/B2 shipped via PR #127 — `Deps` suffix; B3 superseded by PR #144 + this PR; append `## Implementation log` + `## Deviations from plan` + `## Verification`)
  - `docs/plans/shipped/lua-host-friend-drop.md` (append `## Implementation log` + `## Deviations from plan` + `## Verification`)
  - `docs/plans/active/_plan-locks.md` (this entry + Track B status flip)
- **Read-only adjacency**: `Source_Core/src/AppController_LuaBindings.cpp`, `Source_Core/src/AppController_LuaBindingsCore.cpp` (verification no friend-channel breakage)
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — dispatched.
- **Cleared by**: TBD PR.

### test-suite-expansion-completion · Phase-5-preflight · mcp-jsonrpc-pure-tu-split · status: shipped (PR #141 merged at cfab599)

- **Branch**: `feat/mcp-jsonrpc-pure-tu-split` (deleted)
- **Owner agent**: `mcp-toolsmith`
- **Originating plan**: backlog entry `2026-05-16 · mcp-toolsmith · [infra] — MCP wire-protocol pure logic entombed in cpr/httplib-tainted lambda`.
- **Claimed write set**:
  - `Plugins/Mcp/McpJsonRpcPure.h` (NEW)
  - `Plugins/Mcp/McpJsonRpcPure.cpp` (NEW)
  - `Plugins/Mcp/McpPlugin.cpp` (move-out + using-decls only; zero semantic change)
  - `CMakeLists.txt` (add `McpJsonRpcPure.cpp` to `SmatchetPlugin_Mcp` + `SmatchetPlugin_Mcp_DX12`)
  - `docs/plans/active/_plan-locks.md`
  - `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`
  - `docs/plans/shipped/test-suite-expansion-completion.md` (impl-log appendix)
- **Read-only adjacency**: `Plugins/Mcp/McpPlugin.h`, `Source_Core/include/SmatchetDefaults.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #141 at sha cfab599. Pure JSON-RPC surface (12 exported helpers in `smatchet::mcp::pure`) is link-clean for Phase 5 tests.
- **Cleared by**: PR `#141` merged at `cfab599`.

### test-suite-expansion-completion · Phase-5-redispatch · mcp-json-rpc-harness · status: shipped (PR #142 merged at d0b1f12)

- **Branch**: `feat/test-phase-5-mcp-json-rpc` (deleted)
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Phase 5
- **Claimed write set**:
  - `tests/Plugins/Mcp/McpRequestParser.test.cpp` (NEW)
  - `tests/Plugins/Mcp/McpEnvelope.test.cpp` (NEW)
  - `tests/Plugins/Mcp/McpToolSchemas.test.cpp` (NEW)
  - `tests/Plugins/Mcp/McpDispatch.test.cpp` (NEW)
  - `tests/CMakeLists.txt`
  - `docs/plans/active/_plan-locks.md`
  - `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`
  - `docs/plans/shipped/test-suite-expansion-completion.md` (impl-log appendix)
- **Read-only adjacency**: `Plugins/Mcp/McpJsonRpcPure.h`, `Plugins/Mcp/McpJsonRpcPure.cpp`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #142 at sha d0b1f12.
- **Cleared by**: PR `#142` merged at `d0b1f12`.

### test-suite-expansion-completion · Phase-6 · lua-bindings-rig · status: shipped (PR #143 merged at ba1302e)

- **Branch**: `feat/test-phase-6-lua-bindings` (merged)
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Phase 6
- **Claimed write set**: see prior entry (preserved for audit)
- **InitLuaCore classification**: Class C — `AppController_LuaBindings.cpp:32` `#include "imgui.h"` + `:766` `state["__smatchet_app"] = this` + glue functions in `smatchet_lua_init_detail::` resolve `__smatchet_app` back to live `AppController*`. Binding TU is unusable as a test link target without production refactor. LuaBindings.test.cpp deferred; sandbox + timeout + stubs-compile shipped this slice.
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — sandbox + timeout + stubs-compile shipped (14 cases / 99 assertions). LuaBindings.test.cpp deferred — unblocker (this slice) in flight.
- **Cleared by**: PR `#143` merged at `ba1302e`.

### test-suite-expansion-completion · Phase-6-unblocker · lua-bindings-host-interface-lift · status: shipped (PR #144 merged at 7e6762d)

- **Branch**: `feat/lua-bindings-host-interface-lift` (deleted)
- **Owner agent**: `lua-binder`
- **Originating plan**: backlog entry `2026-05-16 · lua-binder · [infra]` in [`docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`](../../self-improvement/AGENT_SELF_IMPROVEMENT.md)
- **Claimed write set**:
  - `Source_Core/include/ILuaBindingHost.h` (NEW)
  - `Source_Core/src/AppController_LuaBindingsCore.cpp` (NEW)
  - `Source_Core/src/AppController_LuaBindings.cpp` (lift out 11 glues + InitLuaCore body)
  - `Source_Core/include/AppController.h` (add `: public ILuaBindingHost` + override declarations)
  - `CMakeLists.txt` (register new TU + `-mcmodel=large` source-property)
  - `docs/plans/active/_plan-locks.md`
  - `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` (flip top entry to applied)
  - `docs/plans/shipped/test-suite-expansion-completion.md` (impl-log appendix)
- **Read-only adjacency**: `Source_Core/src/AppController_LuaStubs.cpp`, `tests/Lua/LuaSandbox.test.cpp` (sandbox closure invariant regression gate)
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #144 at sha 7e6762d. Phase 6b (LuaBindings.test.cpp round-trip) unblocked.
- **Cleared by**: PR `#144` merged at `7e6762d`.

### test-suite-expansion-completion · Phase-6b · lua-bindings-roundtrip · status: shipped (PR #145 merged at d125b36)

- **Branch**: `feat/test-phase-6b-lua-bindings-roundtrip` (deleted)
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Phase 6 (Phase 6b — Lua bindings roundtrip deferred from PR #143)
- **Claimed write set**:
  - `tests/Lua/LuaBindings.test.cpp` (NEW)
  - `tests/support/FakeLuaBindingHost.h` (NEW)
  - `tests/Lua/CMakeLists.txt` (append-only)
  - `docs/plans/active/_plan-locks.md`
  - `docs/plans/shipped/test-suite-expansion-completion.md` (impl-log appendix)
  - `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` (close any TBD PR placeholder left by PR #144)
- **Read-only adjacency**: `Source_Core/include/ILuaBindingHost.h`, `Source_Core/src/AppController_LuaBindingsCore.cpp`, `tests/support/LuaHostFixture.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #145 at sha d125b36. Phase 7 (screenshot diff) unblocked + dispatched.
- **Cleared by**: PR `#145` merged at `d125b36`.

### test-suite-expansion-completion · Phase-9 · coverage-gates · status: shipped (PR #148 merged at 039d286)

- **Branch**: `feat/test-phase-9-coverage-gates` (deleted)
- **Owner agent**: `build-doctor`
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Phase 9
- **Claimed write set**:
  - `scripts/dev/coverage.sh` (NEW — Windows-OpenCppCoverage-first wrapper; inline POSIX `lcov+gcov` fallback documented in header)
  - `scripts/dev/coverage-delta-gate.sh` (NEW — per-PR `Source_Core/` change without test delta → exit 1)
  - `.github/workflows/coverage.yml` (NEW — advisory coverage capture + Cobertura artifact)
  - `.github/workflows/coverage-gate.yml` (NEW — hard-blocking test-delta gate from day 1; `tests-out-of-band` label dismisses)
  - `CMakePresets.json` (append `ninja-test-msvc` extending `ninja-test-msvc` with gcov instrumentation)
  - `docs/plans/active/_plan-locks.md` (this self-status flip + Phase-7 flip)
  - `docs/plans/shipped/test-suite-expansion-completion.md` (impl-log + deviations + verification appendices)
  - `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` (close TBD placeholder on Phase-7 entry; optionally file follow-up entries for OpenCppCoverage CI install + threshold-flip + PR template addition)
- **Read-only adjacency**: `.github/workflows/build-and-test.yml` (pattern reference only), `cmake/Sanitizers.cmake` (helper convention reference)
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #148 at sha `039d286`. Final phase of `test-suite-expansion-completion` plan; plan doc moved to `docs/plans/shipped/` with full Outcome table in the same chore PR.
- **Cleared by**: PR `#148` merged at `039d286`.

### test-suite-expansion-completion · Phase-7 · screenshot-diff · status: shipped (PR #146 merged at d857310)

- **Branch**: `feat/test-phase-7-screenshot-diff` (deleted)
- **Owner agent**: `test-author`
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Phase 7
- **Claimed write set**:
  - `Source_Core/include/SmatchetUiSession.h` (add `requestCommandPaletteOpen` + `requestCommandPaletteFilter` flag pair so the palette scenario can open + pre-filter the modal without touching `SmatchetUI`'s private `commandPalette_`)
  - `Source_Core/src/SmatchetUI.cpp` (consume the new flags once per frame right before `commandPalette_.Draw`)
  - `Source_Core/include/Commands/Scenarios/DockGapSentinelScenario.h` (NEW — factory entry-point)
  - `Source_Core/src/Commands/Scenarios/DockGapSentinelScenario.cpp` (NEW — drives default dock + reset, screenshot trigger)
  - `Source_Core/include/Commands/Scenarios/CommandPaletteFuzzyScenario.h` (NEW)
  - `Source_Core/src/Commands/Scenarios/CommandPaletteFuzzyScenario.cpp` (NEW)
  - `Source_Core/src/AppController.cpp` (append two `scenarioRunner_->RegisterFactory` lines next to the existing trio at lines 1290-1305)
  - `tests/support/GoldenImage.h` (NEW — header-only PPM reader + per-channel L∞ diff)
  - `tests/support/ScreenshotDiffMain.cpp` (NEW — tiny CLI helper compiled by the bash script via the host's g++ for the diff step; PascalCase to dodge the repo's `.gitignore` `screenshot_*` rule for capture outputs)
  - `tests/golden/dock-gap-sentinel.ppm` (NEW — bootstrapped via `--bootstrap` first run; 1280x720 P6 PPM)
  - `tests/golden/command-palette-fuzzy.ppm` (NEW — bootstrapped via `--bootstrap` first run; 1280x720 P6 PPM)
  - `tests/golden/README.md` (NEW — bootstrap protocol + regeneration recipe)
  - `scripts/dev/test-screenshot-diff.sh` (NEW — bash driver, auto-enrolled by test-all.sh)
  - `.github/workflows/build-and-test.yml` (append a `continue-on-error: true` screenshot-diff step that no-ops cleanly on headless runners)
  - `docs/plans/active/_plan-locks.md` (this self-status flip)
  - `docs/plans/shipped/test-suite-expansion-completion.md` (impl-log + deviations + verification appendices)
  - `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` (DX12 backbuffer readback follow-up + headless-CI display-server follow-up)
- **Read-only adjacency**: `Source_Core/src/Commands/Builtin/BuiltinCommands_Debug.cpp` (debug.window.screenshot already wired), `Source_Core/src/Commands/Builtin/BuiltinCommands_Scenario.cpp`, `Source_Core/src/Commands/Scenarios/UiTestScenario.cpp`, `Target_Standalone/main.cpp` (PPM writer already present at line 569).
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #146 at sha d857310. 2 scenarios (`dock-gap-sentinel`, `command-palette-fuzzy`) + GoldenImage.h + bash driver + advisory CI step (`continue-on-error: true` until 2026-05-30). Auto-bootstrap on first run; subsequent runs gate at L∞ <= 4.
- **Cleared by**: PR `#146` merged at `d857310`.

### test-suite-expansion · phases 2–9 · status: abandoned (superseded umbrella)

- **Originally claimed** by machine-A under the older `test-suite-expansion.md` umbrella. Superseded by `test-suite-expansion-completion.md` per-phase claims (Phase 1 / Phase 4 already shipped; Phase 5 abandoned + this pre-flight unblocker now in-flight). Pruned in coordination with the Phase 5 unblocker so the lock file reflects the real per-phase state.
- **Cleared by**: superseded — see this plan's per-phase entries.

### test-suite-expansion-completion · Phase-4 · config-schema-migration · status: shipped (PR #134 merged at 3e19f93)

- **Branch**: `feat/test-phase-4-config-migration` (deleted)
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Phase 4
- **Last update**: 2026-05-16 — merged via PR #134 at sha 3e19f93. 21 cases / 99 assertions on Config surface. Shared `tests/support/TestEnvGuard.h` shipped — Phase 5+ can consume.
- **Cleared by**: PR `#134` merged at `3e19f93`.

### test-suite-expansion-completion · Phase-5 · mcp-json-rpc-harness · status: abandoned (blocked on production TU split)

- **Branch**: `feat/test-phase-5-mcp-json-rpc` (never pushed — agent stopped before commits)
- **Owner agent**: `test-rig` (test-only scope per Phase 5 plan)
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Phase 5
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — user stopped agent at session end (wrap-up). Agent's discovery phase confirmed Phase 5 is **blocked** by the same pattern as the P4Blame deferral: every pure helper (`BuildRunLuaToolEntry`, `BuildRunLuaSummary`, `BuildToolCallSummary`, `ExtractJsonRpcErrorMessage`, `Base64Encode`, `NormalizeDomain`, `IsLoopbackAddress`, `ConstantTimeStringEquals`, `IsAllowedAttachmentHost`) lives in an anonymous namespace inside a `Plugins/Mcp/*.cpp` whose top of file pulls `winsock2` + `httplib` + `cpr`. Tests cannot link the unit without dragging banned deps. Production-side TU split needed first (same recipe as `P4BlameParse`, `TrackerLabelsPure`, etc).
- **Cleared by**: blocked — see backlog entry `2026-05-16 · mcp-toolsmith · [infra] — MCP wire-protocol logic entombed in cpr/httplib-tainted lambda`.

### backend-audit-trail-per-event-path · status: shipped

- **Branch**: `feat/audit-trail-per-event-path`
- **Owner agent**: orchestrator
- **Originating plan**: backlog entries 2026-05-16 `security-review` + `offline-sync` in [`docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`](../../self-improvement/AGENT_SELF_IMPROVEMENT.md)
- **Claimed write set**:
  - `Source_Core/src/BackendAuditTrail.cpp` (writer re-resolves `GetAuditFilePath()` per-event)
  - `tests/Source_Core/BackendAuditTrail.test.cpp` (add runtime-dir-change case; existing TEST_CASE workaround drops)
  - `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` (status flip)
- **Read-only adjacency**: `Source_Core/include/BackendAuditTrail.h`, `Source_Core/include/ConfigManager.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #108 at sha 98ed9ea.
- **Cleared by**: PR `#108` merged at `98ed9ea`.

### cached-ticket-types-header-split · status: shipped

- **Branch**: `feat/cached-ticket-types-header-split`
- **Owner agent**: orchestrator
- **Originating plan**: backlog entry 2026-05-16 `test-rig · [infra]` in [`docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`](../../self-improvement/AGENT_SELF_IMPROVEMENT.md)
- **Claimed write set**:
  - `Source_Core/include/CachedTicketTypes.h` (NEW)
  - `Source_Core/include/LocalCacheManager.h`
  - `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`
- **Read-only adjacency**: 20 callers of `LocalCacheManager.h` (no code edit — re-include keeps API)
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — shipped on develop at sha 8724b8b (branch deleted).
- **Cleared by**: merged to develop at `8724b8b`.

### code-review-backlog · A3 · required-field-glyph · status: shipped (PR #113 merged at 42b18a4)

- **Branch**: `feat/required-field-ui-glyph` (deleted)
- **Owner agent**: `tracker-backend`
- **Originating plan**: [`backlog/BACKLOG_CODE_REVIEW.md`](../../../backlog/BACKLOG_CODE_REVIEW.md) § A3
- **Claimed write set**:
  - `Source_Core/src/TicketFieldEditor.cpp`
  - `Source_Core/src/SmatchetNewIssueDraftUi.cpp`
  - `Source_Core/src/SmatchetLocalization.cpp` (agent corrected: baseline strings live in C++ table, not `Locales/*.json`)
  - `backlog/BACKLOG_CODE_REVIEW.md` (status flip A3)
- **Read-only adjacency**: `Source_Core/include/TrackerFieldCatalog.h`, `Source_Core/include/TicketFieldEditor.h`, `Source_Core/include/SmatchetNewIssueDraftUi.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR [#113](https://github.com/alexandrosk0/Smatchet/pull/113) at sha `42b18a4`.
- **Cleared by**: PR `#113` merged at `42b18a4`.

### code-review-backlog · C7 · grid-pushcliprect-audit · status: shipped (PR #115 merged at d6cb833)

- **Branch**: `feat/grid-pushcliprect-audit` (deleted)
- **Owner agent**: `grid-engine`
- **Originating plan**: [`backlog/BACKLOG_CODE_REVIEW.md`](../../../backlog/BACKLOG_CODE_REVIEW.md) § C7
- **Claimed write set**:
  - `Source_Core/src/SmatchetActiveProjectGridUi.cpp` (lines 843, 905, 930 — remove `PushClipRect`/`PopClipRect` pairs if redundant)
  - `backlog/BACKLOG_CODE_REVIEW.md` (status flip C7)
- **Read-only adjacency**: `Source_Core/include/SmatchetActiveProjectGridUi.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR [#115](https://github.com/alexandrosk0/Smatchet/pull/115) at sha `d6cb833`.
- **Cleared by**: PR `#115` merged at `d6cb833`.

### code-review-backlog · C2 · markdown-emitinlinetext-scratch · status: shipped (PR #117 merged at 4eab183)

- **Branch**: `feat/markdown-emitinlinetext-scratch` (deleted)
- **Owner agent**: orchestrator (direct — small allocator-only change, no header touch)
- **Originating plan**: [`backlog/BACKLOG_CODE_REVIEW.md`](../../../backlog/BACKLOG_CODE_REVIEW.md) § C2
- **Claimed write set**:
  - `Source_Core/src/MarkdownConvert.cpp` (EmitInlineText at line 723 only)
  - `backlog/BACKLOG_CODE_REVIEW.md` (status flip C2)
- **Read-only adjacency**: none.
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR [#117](https://github.com/alexandrosk0/Smatchet/pull/117) at sha `4eab183`.
- **Cleared by**: PR `#117` merged at `4eab183`.

### code-review-backlog · C3 · plane-fetchissueeditmeta-broaden · status: shipped (PR #118 merged at df3836b)

- **Branch**: `feat/plane-fetchissueeditmeta-broaden` (deleted)
- **Owner agent**: orchestrator (direct — one-line list expansion)
- **Originating plan**: [`backlog/BACKLOG_CODE_REVIEW.md`](../../../backlog/BACKLOG_CODE_REVIEW.md) § C3
- **Claimed write set**:
  - `Source_Core/src/PlaneFieldCatalog.cpp` (FetchIssueEditMeta at line 492 only — broaden hardcoded set; root cause "Plane has no per-issue capability endpoint" documented inline)
  - `backlog/BACKLOG_CODE_REVIEW.md` (status flip C3 → 🟡 partial; real-permissions query deferred)
- **Read-only adjacency**: `Source_Core/include/PlaneClient.h`.
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR [#118](https://github.com/alexandrosk0/Smatchet/pull/118) at sha `df3836b`.
- **Cleared by**: PR `#118` merged at `df3836b`.

### code-review-backlog · B4 · plane-fetchissuesforkeys-filter · status: shipped (PR #116 merged at 23afa9c)

- **Branch**: `feat/plane-fetchissuesforkeys-filter` (deleted)
- **Owner agent**: `tracker-backend` (orchestrator-direct after isolated worktrees thrashed twice on API 500)
- **Originating plan**: [`backlog/BACKLOG_CODE_REVIEW.md`](../../../backlog/BACKLOG_CODE_REVIEW.md) § B4
- **Claimed write set**:
  - `Source_Core/src/PlaneIssueSearch.cpp` (FetchIssuesForKeys only — file split from `PlaneClient.cpp` since the backlog entry was written)
  - `backlog/BACKLOG_CODE_REVIEW.md` (status flip B4 → 🟡 partial; server-side `sequence_id__in` filter deferred as B4-v2)
- **Read-only adjacency**: `Source_Core/include/PlaneClient.h`, `Source_Core/include/ITrackerClient.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR [#116](https://github.com/alexandrosk0/Smatchet/pull/116) at sha `23afa9c`.
- **Cleared by**: PR `#116` merged at `23afa9c`.

### test-suite-expansion-completion · wave-A1 · callstack-adversarial-subcases · status: shipped

- **Branch**: `feat/test-callstack-adversarial`
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Carry-over A
- **Claimed write set**:
  - `tests/Source_Core/CallstackParser.test.cpp`
  - `docs/plans/shipped/test-suite-expansion.md` (impl-log appendix)
  - `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` (status flip on `code-review+security-review · [test]` entry)
- **Read-only adjacency**: `Source_Core/src/CallstackParser.cpp`, `Source_Core/include/CallstackParser.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #112 at sha effda92.
- **Cleared by**: PR `#112` merged at `effda92`.

### test-suite-expansion-completion · wave-A1 · p4blame-parse-tu-split · status: shipped

- **Branch**: `feat/p4blame-parse-tu-split`
- **Owner agent**: `test-rig` (TU-split pre-authorised per AGENTS.md applied rule)
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Carry-over B
- **Claimed write set**:
  - `Source_Core/include/P4BlameParse.h` (NEW)
  - `Source_Core/src/P4BlameParse.cpp` (NEW)
  - `Source_Core/src/P4Blame.cpp` (call-site rewire of the four lifted helpers only — no semantic change)
  - `tests/Source_Core/P4BlameParse.test.cpp` (NEW)
  - `tests/CMakeLists.txt`
  - `docs/plans/shipped/test-suite-expansion.md` (impl-log appendix)
  - `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` (status flip on `test-rig · [infra] — Phase 2 P4BlameParse deferred`)
- **Read-only adjacency**: `Source_Core/include/P4Blame.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #111 at sha 52832d0.
- **Cleared by**: PR `#111` merged at `52832d0`.

### test-suite-expansion-completion · wave-A2 · tracker-labels-pure-tu · status: shipped (PR #114 merged at 59282a7)

- **Branch**: `feat/tracker-labels-pure-tu`
- **Owner agent**: `test-rig` (TU-split pre-authorised)
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Carry-overs C1
- **Claimed write set**:
  - `Source_Core/include/TrackerLabelsPure.h` (NEW)
  - `Source_Core/src/TrackerLabelsPure.cpp` (NEW)
  - `Source_Core/src/TrackerLabelsEditor.cpp` (call-site rewire of pure helpers only — no semantic change)
  - `tests/Source_Core/TrackerLabelsPure.test.cpp` (NEW)
  - `tests/CMakeLists.txt`
  - `docs/plans/shipped/test-suite-expansion.md` (impl-log appendix)
  - `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` (status flip on Phase 1 deferral entry)
  - `docs/plans/active/_plan-locks.md` (self-status flips)
- **Read-only adjacency**: `Source_Core/include/TrackerLabelsEditor.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — agent errored API-500 mid-run; orchestrator recovered worktree state, verified gates, committed + pushed + merged.
- **Cleared by**: see PR table below.

### test-suite-expansion-completion · wave-A2 · tracker-datetime-pure-tu · status: shipped (PR #119 merged at 9fc5f70)

- **Branch**: `feat/tracker-datetime-pure-tu`
- **Owner agent**: `test-rig` (TU-split pre-authorised)
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Carry-overs C2
- **Claimed write set**:
  - `Source_Core/include/TrackerDateTimePure.h` (NEW)
  - `Source_Core/src/TrackerDateTimePure.cpp` (NEW)
  - `Source_Core/src/TrackerDateTimeFieldEditor.cpp` (call-site rewire only)
  - `tests/Source_Core/TrackerDateTimePure.test.cpp` (NEW)
  - `tests/CMakeLists.txt`
  - `docs/plans/shipped/test-suite-expansion.md` (impl-log appendix)
  - `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` (status flip on Phase 1 deferral entry)
  - `docs/plans/active/_plan-locks.md` (self-status flips)
- **Read-only adjacency**: `Source_Core/include/TrackerDateTimeFieldEditor.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — agent errored API-500 mid-run; orchestrator recovered worktree state, verified gates, committed + pushed + merged.
- **Cleared by**: see PR table below.

### test-suite-expansion-completion · wave-A2 · tracker-payload-pure-tu · status: shipped (PR #121 merged at 39f91de)

- **Branch**: `feat/tracker-payload-pure-tu`
- **Owner agent**: `test-rig` (TU-split pre-authorised)
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Carry-overs C3
- **Claimed write set**:
  - `Source_Core/include/TrackerFieldPayloadPure.h` (NEW)
  - `Source_Core/src/TrackerFieldPayloadPure.cpp` (NEW)
  - `Source_Core/src/TrackerFieldPayload.cpp` (call-site rewire only — `JiraClient.h` stays in production TU)
  - `tests/Source_Core/TrackerFieldPayloadPure.test.cpp` (NEW)
  - `tests/CMakeLists.txt`
  - `docs/plans/shipped/test-suite-expansion.md` (impl-log appendix)
  - `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` (status flip on Phase 1 deferral entry)
  - `docs/plans/active/_plan-locks.md` (self-status flips)
- **Read-only adjacency**: `Source_Core/include/TrackerFieldPayload.h`, `Source_Core/include/JiraClient.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — agent errored API-500 mid-run; orchestrator recovered worktree state, verified gates, committed + pushed + merged.
- **Cleared by**: see PR table below.

### test-suite-expansion-completion · wave-A2 · tracker-field-catalog-pure-tu · status: shipped (PR #122 merged at 5ce8def)

### test-suite-expansion-completion · PR-D · offline-queue-deps-interface · status: shipped (PR #127 merged at b5fc194)

- **Branch**: `feat/offline-queue-deps-interface` (deleted)
- **Owner agent**: `offline-sync`
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Per-slice scoping § PR D
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #127 at sha b5fc194. Track B (`large-files-and-phase-2`) on-hold gate now releases.
- **Cleared by**: PR `#127` merged at `b5fc194`.

### test-suite-expansion-completion · PR-E · offline-queue-runtime-tests · status: shipped (PR #131 merged at e35794d)

- **Branch**: `feat/offline-queue-runtime-tests` (deleted)
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Per-slice scoping § PR E
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #131 at sha e35794d. Required orchestrator-side dedup after rebase: test-side `IsTrackerTransportErrorText` mirror collided with production once PR F's `TrackerHttpUtils.cpp` joined the test target source list.
- **Cleared by**: PR `#131` merged at `e35794d`.

### test-suite-expansion-completion · PR-F · ticket-sync-service-tests · status: shipped (PR #130 merged at a618a2f)

- **Branch**: `feat/ticket-sync-service-tests` (deleted)
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Per-slice scoping § PR F
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #130 at sha a618a2f. 12 cases / 83 assertions. Case 3 documents a current production bug (empty fetch in full-sync deletes all rows) — separate fix-PR pending under `offline-sync` follow-up (backlog entry filed).
- **Cleared by**: PR `#130` merged at `a618a2f`.

- **Branch**: `feat/tracker-field-catalog-pure-tu`
- **Owner agent**: `test-rig` (TU-split pre-authorised)
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion-completion.md`](../shipped/test-suite-expansion-completion.md) § Carry-overs C4
- **Claimed write set**:
  - `Source_Core/include/TrackerFieldCatalogPure.h` (NEW)
  - `Source_Core/src/TrackerFieldCatalogPure.cpp` (NEW)
  - `Source_Core/src/TrackerFieldCatalog.cpp` (call-site rewire only — `JiraClient.h` stays in production TU)
  - `tests/Source_Core/TrackerFieldCatalogPure.test.cpp` (NEW)
  - `tests/CMakeLists.txt`
  - `docs/plans/shipped/test-suite-expansion.md` (impl-log appendix)
  - `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` (status flip on Phase 1 deferral entry)
  - `docs/plans/active/_plan-locks.md` (self-status flips)
- **Read-only adjacency**: `Source_Core/include/TrackerFieldCatalog.h`, `Source_Core/include/JiraClient.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — agent errored API-500 mid-run; orchestrator recovered worktree state, verified gates, committed + pushed + merged.
- **Cleared by**: see PR table below.

### test-suite-expansion · phase 1 · status: shipped (PR #103 merged at fdac8ff)

- **Branch**: `feat/test-phase-1-tracker-pure-logic`
- **Owner agent**: orchestrator (autonomous multi-phase mode per the plan's § Execution contract)
- **Originating plan**: [`docs/plans/shipped/test-suite-expansion.md`](../shipped/test-suite-expansion.md) § Phase 1
- **Claimed write set**:
  - `Source_Core/include/IssueCreatePipelineHelpers.h` (NEW)
  - `Source_Core/src/IssueCreatePipeline.cpp`
  - `Source_Core/src/IssueCreatePipelineHelpers.cpp` (NEW)
  - `Source_Core/src/IssueDraft.cpp`
  - `Source_Core/src/TrackerFieldValueParser.cpp`
  - `tests/CMakeLists.txt`
  - `tests/Source_Core/IssueCreatePipeline.test.cpp` (NEW)
  - `tests/Source_Core/IssueDraft.test.cpp` (NEW)
  - `tests/Source_Core/TrackerFieldValueParser.extended.test.cpp` (NEW)
  - `tests/Source_Core/TrackerFieldValueUtils.test.cpp` (NEW)
  - `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`
  - `docs/plans/shipped/test-suite-expansion.md` (impl-log appendix)
- **Read-only adjacency**: `Source_Core/include/IssueDraft.h`, `Source_Core/include/TrackerFieldValueParser.h`, `Source_Core/include/IssueCreatePipeline.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR [#103](https://github.com/alexandrosk0/Smatchet/pull/103) at sha `fdac8ff`.
- **Cleared by**: PR `#103` merged at `fdac8ff`.

### test-suite-expansion · phases 2–9 · status: abandoned (superseded by `test-suite-expansion-completion.md` per-phase claims)

- **Original Owner**: orchestrator (autonomous; see [`docs/plans/shipped/test-suite-expansion.md`](../shipped/test-suite-expansion.md) § Execution contract).
- **Superseded by**: Phase-by-phase claims under `test-suite-expansion-completion.md`. Phases 1 + 4 shipped; Phase 5 abandoned then unblocked by the `mcp-jsonrpc-pure-tu-split` slice above. The umbrella claim was never honoured across the 8 phases that actually shipped against develop.
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — converted `claimed` → `abandoned` by the Phase-5 pre-flight unblocker so the lock file matches reality.
- **Cleared by**: superseded.

### large-files-and-phase-2 · Track B (B1–B3 + fix-up) · status: shipped via PR #127 (B1+B2) + PR #144 (B3 superseded) + this PR (friend drop)

- **Branch**: TBD per slice (B1: `claude/offline-queue-icache-access`, B2: TBD, B3: TBD)
- **Owner agent**: `offline-sync` (B1, B2), `lua-binder` (B3)
- **Originating plan**: [`docs/plans/shipped/large-files-and-phase-2.md`](../shipped/large-files-and-phase-2.md) § Track B
- **Reason on-hold (historical)**: overlapping write set with `test-suite-expansion` phases 2-9 (`TicketSyncService.cpp`, `ConfigManager.cpp`, `AppController.h`, `tests/CMakeLists.txt`). Resuming Track B before those test phases landed would have forced a multi-way rebase that defeated both efforts. Resume gate cleared by PR #148 + #149.
- **Claimed write set on resume** (preview — re-asserted at resume time):
  - B1: shipped as `IOfflineQueueDeps` (PR #127) — `Deps` suffix naming, not `Access`/`Host`. Behaviour equivalent.
  - B2: shipped as `ITicketSyncDeps` (PR #127) — `Deps` suffix naming, not `Access`/`Host`. Behaviour equivalent.
  - B3a-d: **superseded** by PR #144 (`ILuaBindingHost` TU lift — opposite architectural choice; AppController stays binding owner). Residual friend drops this PR via dead-code removal.
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — closed via lua-host-friend-drop slice (this PR). All three friend-class declarations now removed from `AppController.h`.
- **Cleared by**: PR #127 (`b5fc194`) + PR #144 (`7e6762d`) + TBD (this PR).

### ai-assistant-side-panel · Phase A-narrowed · status: shipped (PR #140 merged at eeea501)

- **Branch**: `feat/ai-assistant-side-panel` (plan recovered from dangling 84913a8 → a39097c)
- **Owner agent**: orchestrator (direct — provider-pluggable C++14 skeleton)
- **Originating plan**: [`docs/plans/shipped/ai-assistant-side-panel.md`](../shipped/ai-assistant-side-panel.md) § Phase A
- **Claimed write set** (narrowed — ConfigManager fields + tests deferred to Phase A' until `test-suite-expansion` umbrella releases `Source_Core/src/ConfigManager*.cpp` + `tests/**`):
  - `Source_Core/include/IAiClient.h` (NEW)
  - `Source_Core/include/AiTypes.h` (NEW)
  - `Source_Core/include/AiClientFactory.h` (NEW)
  - `Source_Core/src/AiClientFactory.cpp` (NEW)
  - `Source_Core/include/OpenAiClient.h` (NEW)
  - `Source_Core/src/OpenAiClient.cpp` (NEW)
  - `Source_Core/include/AiSseParser.h` (NEW)
  - `Source_Core/src/AiSseParser.cpp` (NEW)
  - `Source_Core/include/NetworkUsageTracker.h` (re-add `HttpTrafficKind::Ai` + `aiRequests/aiUploadBytes/aiDownloadBytes` snapshot fields)
  - `Source_Core/src/NetworkUsageTracker.cpp`
  - `Source_Core/src/TrackerHttpUtils.cpp` (one-line `Record(HttpTrafficKind::Tracker, …)` update)
  - `Source_Core/src/JiraIssueMutation.cpp` (one-line `Record` update)
  - `Source_Core/src/FieldCatalogCache.cpp` (one-line `Record` update)
  - `CMakeLists.txt` (add `option(SMATCHET_WITH_AI "..." ON)` + new sources to `CORE_SOURCES`)
- **Read-only adjacency**: `Source_Core/include/AppController.h`, `Source_Core/include/MainThreadDispatcher.h`
- **Deferred to Phase A' (gated on test-suite-expansion umbrella release)**:
  - `Source_Core/include/ConfigManager.h` + `Source_Core/src/ConfigManager.cpp` (Ai field set + DPAPI key protection)
  - `tests/CMakeLists.txt` + `tests/Source_Core/AiSseParser.test.cpp` + `tests/Source_Core/AiClientFactory.test.cpp` (doctest coverage)
- **Started**: 2026-05-16
- **Last update**: 2026-05-17 — Phase A-narrowed shipped via PR #140 at sha eeea501. Phase A' (ConfigManager Ai fields + doctests) now dispatched on `feat/ai-config-fields-and-tests` after `test-suite-expansion` umbrella released (PR #149).
- **Cleared by**: PR `#140` merged at `eeea501`.

### ai-assistant-side-panel · Phase A' · ai-config-fields-and-tests · status: shipped (PR #157 merged at a7cd940)

- **Branch**: `feat/ai-config-fields-and-tests`
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/plans/shipped/ai-assistant-side-panel.md`](../shipped/ai-assistant-side-panel.md) § File-level changes (ConfigManager rows) + § Pending follow-ups (Phase A' row)
- **Claimed write set**:
  - `Source_Core/include/ConfigManager.h` (append 17 `TrackerConfig` Ai fields + `#include "AiTypes.h"`)
  - `Source_Core/src/ConfigManager.cpp` (serialize + deserialize of new fields; DPAPI on `AiApiKey` + `AiAnthropicApiKey`; clamp `AiProviderKind`; `%LOCALAPPDATA%/Smatchet/agents.md` default at Load time)
  - `tests/Source_Core/AiSseParser.test.cpp` (NEW — ≥ 8 cases / ≥ 40 assertions, ≥ 2 `[high-risk]`)
  - `tests/Source_Core/AiClientFactory.test.cpp` (NEW — ≥ 4 cases / ≥ 20 assertions)
  - `tests/CMakeLists.txt` (append: 2 new tests + `AiSseParser.cpp` + `AiClientFactory.cpp` + `OpenAiClient.cpp`)
  - `docs/plans/active/_plan-locks.md` (this entry)
  - `docs/plans/shipped/ai-assistant-side-panel.md` (append Implementation log row + Deviations bullets + extend Verification)
- **Read-only adjacency**: `Source_Core/include/AiTypes.h`, `Source_Core/include/AiClientFactory.h`, `Source_Core/include/AiSseParser.h`, `Source_Core/include/IAiClient.h`, `Source_Core/src/AiClientFactory.cpp`, `Source_Core/src/AiSseParser.cpp`, `Source_Core/src/ConfigManager_PathUtils.cpp` (`GetPlatformSharedUserDataDirectory` reuse)
- **Started**: 2026-05-17
- **Last update**: 2026-05-17 — merged via PR [#157](https://github.com/alexandrosk0/Smatchet/pull/157) at sha `a7cd940`. SmatchetTests aggregate post-merge: 331 cases / 1745 assertions. ConfigManager Ai field set + DPAPI on `AiApiKey` + `AiAnthropicApiKey` + `AiSseParser` + `AiClientFactory` doctests landed. Phase B unblocked.
- **Cleared by**: PR `#157` merged at `a7cd940`.

## Shipped recent entries

(Pruned after ~14 days. Kept here briefly to give concurrent agents recent context.)

### large-files-and-phase-2 · Track A · status: shipped

- A5 code → revival PR pending on `feat/app-controller-lua-types-a5` (PR #93 shipped only the plan doc; the `AppController_LuaTypes.h` extraction commit was not included in that merge)
- A3 → PR [#97](https://github.com/alexandrosk0/Smatchet/pull/97) (merged 2026-05-16)
- A2 → PR [#98](https://github.com/alexandrosk0/Smatchet/pull/98) (merged 2026-05-16)
- A1 → PR [#100](https://github.com/alexandrosk0/Smatchet/pull/100) (pending)
- A4 → PR [#101](https://github.com/alexandrosk0/Smatchet/pull/101) (pending)
- Plan revision → PR [#102](https://github.com/alexandrosk0/Smatchet/pull/102) (pending)

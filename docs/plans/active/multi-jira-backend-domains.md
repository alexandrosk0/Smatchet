<!-- index-summary: Multiple Jira origins with inherited credentials and a Views domain picker. -->
# Plan — Multiple Jira backend domains

> **Slug**: `multi-jira-backend-domains`
>
> **Status**: `active`
>
> **Usage**: product feature requested by the user: extra Jira sites, credentials that default to the first instance, domain switcher in Views - Jira.

## Context

A user can need more than one Jira Cloud/Server origin (studio vs publisher, two Atlassian sites). Today `TrackerConfig` holds a single `Domain` / `Email` / `ApiToken` triple, Views is keyed by tracker kind (`Jira`), and `tickets_v2` is namespaced by that kind — so a second site would either overwrite the first or collide on keys like `PROJ-123`.

After this lands, Preferences can list extra Jira domains; empty email/token on an extra inherit from the first instance; Views - Jira can switch the live origin without leaving the Jira backend kind.

## Approach

Keep one Jira *kind* (`NormalizeViewsBackendKey` stays `"Jira"`). Add `JiraBackendInstance { Domain, Email, ApiToken }` plus `TrackerConfig::JiraBackends` (index 0 = first instance) and `ActiveJiraDomain`.

On disk:
- top-level `domain` / `email` / `token` remain the **first** instance (legacy configs and env `SMATCHET_TRACKER_*` unchanged)
- `jira_backends` is the extra-only array (optional `email` / `token` / `token_enc`)
- `active_jira_domain` is the currently selected host

In memory after `Load()`, `Domain` / `Email` / `ApiToken` are the **resolved live** values for the active instance so existing `JiraClient` paths that `ConfigManager::Load()` or take `cfg.Domain` keep working. `Save` runs `PrepareForPersist` so the first instance is what hits the top-level keys.

Ticket cache namespace (ADR-0018 already sketched `<type>:<profileId>`): first/active-equals-first stays `"Jira"` (no migration); an extra host uses `"Jira:<normalized-host>"`. Views stay under the `"Jira"` bucket — one Views - Jira window, shared JQL/field-sets, domain combo switches origin + cache namespace + recreates `JiraClient` (ListProjects TTL cache is per-client).

Empty extra email/token inherit from `JiraBackends[0]` at resolve time, never by copying secrets onto the extra row on save.

## Files to modify

1. `Source/Core/include/Config/ConfigManager.h` — `JiraBackendInstance` + `JiraBackends` / `ActiveJiraDomain` on `TrackerConfig`.
2. `Source/Core/include/Config/JiraBackendInstancesPure.h` + `Source/Core/src/Config/JiraBackendInstancesPure.cpp` — host normalize, hydrate, inherit resolve, persist prepare, add/remove/select, cache-key. **Grep before naming:** no existing `JiraBackend*` helper TU.
3. `Source/Core/src/Config/ConfigManager.cpp` — `active_jira_domain` in `kStringFields`.
4. `Source/Core/src/Config/ConfigManager_Load.cpp` / `_Save.cpp` / `_Secrets.cpp` — load extras + hydrate + apply live fields; persist first-instance overlay + extras (DPAPI/`token_enc` on Win32/Android).
5. `Source/Core/src/Config/ConfigManager_PathUtils.cpp` — sanitize `jira_backends[].domain`.
6. `Source/Core/src/Sync/TicketSyncService.cpp` (+ header) — recreate Jira client when host changes; stamp `TrackerCacheBackendKey`.
7. `Source/Core/src/AppController_Init.cpp` — InitBackends cache key uses `TrackerCacheBackendKey`.
8. `Source/Core/src/Ui/SmatchetPreferencesUi.cpp` + `SmatchetUiSession.h` + `PreferencesSchema.cpp` + `SmatchetPreferencesUi_detail.h` — extra-sites editor; prefs Domain/Email/Token edit instance 0.
9. `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp` + `SmatchetUI.h` — domain combo in Views - Jira.
10. `Source/Core/src/SmatchetLocalization.cpp` — Views/prefs copy.
11. `tests/Core/JiraBackendInstancesPure.test.cpp` + CMake subset/main lists + `ConfigManager.test.cpp` round-trip + `ConfigStringSanitize.test.cpp` extras domain.
12. `docs/guides/cli.md`, `docs/CONTEXT.md`, `Source/Core/src/Tracker/CONTEXT.md`, `Source/Core/src/Sync/AGENTS.md`.

## Existing utilities reused

- `ConfigManager::NormalizeViewsBackendKey` / `KnownBackendKeys` — kind bucket stays `Jira`.
- `ProtectSecretForConfig` / `UnprotectSecretFieldFromConfig` / `ApplySecretPersist` — extra-row tokens.
- `TicketSyncService::SwapBackendIfTrackerChanged` / `ITrackerBackendFactory::Create` — live client swap (ADR-0012 shared_ptr).
- `FieldCatalogCache::BuildFieldCatalogCacheKey` — already `Jira|<domain>|<project>`.
- `TrimCopyAsciiWhitespace` — prefs trim of extra rows.
- `SmatchetLocalization::Format("window.views_backend", …)` — window title stays `Views - Jira`.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — new helper TU, no whale extraction.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: domain combo is idle unless extras exist; no per-ticket work. Combo draw is a handful of ImGui widgets.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: domain switch calls existing `SyncWithBackend` (async streaming). No new sync I/O on the render path. Config save is the existing worker/direct Save path.
- **Pillar 3 (never crash)**: RAII client swap; duplicate/empty domain rejected; deleting the active extra falls back to instance 0; cache key isolated so two sites cannot clobber `PROJ-123`.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: ImGui Combo/InputText inherit existing keyboard nav.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

1. **PR-fast CI** — N/A for a new idle combo + config resolve; closest existing scenario is tracker-switch (`preferences_tracker_switch` is bucket-E, not PR-fast). No grid hot-path change. Map: no curated scenario names this config/Views chrome.
2. **Pillar 2 static scanner** — no new sync I/O from `ImGui::*`. Domain switch posts the existing worker sync.
3. **Dispatcher drain** — N/A — does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — N/A — no new >100 ms UI-thread stall; sync already toasts "Syncing".
5. **Marker inventory** — N/A — no new `SMATCHET_UI_PERF_SCOPE`.

**Pre-push local check**: Linux container cannot run `perf-run.sh` Windows scenarios; Core unit tests on `ninja-test-linux` instead.

**Override**: none expected.

## Risks / non-goals

- **Cache collision** — mitigation: extra hosts use `Jira:<host>`; first stays `"Jira"`.
- **Persisting inherited secrets onto extras** — mitigation: empty extra email/token stay empty on disk; inherit only in `ApplyActiveLiveFields`.
- **Prefs Domain field showing the live extra** — mitigation: prefs Domain/Email/Token always edit `JiraBackends[0]`.
- **Non-goals**: Plane/GitHub/Linear multi-site; per-domain Views buckets; simultaneous two-Jira grids; Test-connection per extra row (primary Test connection unchanged).

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `JiraBackendInstancesPure.test.cpp` (host normalize, inherit, cache key, add/remove/select, persist-prepare does not leak extra Domain into first). `ConfigManager` Save/Load extras round-trip. `SanitizeHeaderBoundConfigKeys` extras domain. TicketSyncService host-change recreates backend + stamps `Jira:<host>`.
- **Bucket E (ImGui Test Engine)**: N/A this slice — Linux container cannot run the MSVC UI rig; combo is standard ImGui. Manual residue named below.
- **Bash-driver scenario / screenshot / sanitizer**: N/A — no scenario/golden. Sanitizer: existing CI lanes.
- **Build gate**: Linux `ninja-test-linux` Core subset. Dual-target MSVC is CI-only here (`linux-container` tier).
- **Doc validation**: `scripts/dev/test-docs.sh`.
- **Plan stress-test — `grill-with-docs`**: self-grilled against `docs/CONTEXT.md` + Tracker `CONTEXT.md` + ADR-0012/0018. Terms added: **Jira backend instance**, **Active Jira domain**. Cache namespace reuses ADR-0018 `<type>:<profileId>` rather than a new ADR. No new trust-boundary HTTP surface (same `JiraClient` + same tokens).
- **Manual residue**: Views - Jira domain combo + Preferences extra-sites editor on a Windows desktop (cloud agent cannot launch `Smatchet.exe`). Follow-up: bucket-E `preferences_tracker_switch`-style case when a Windows UI-test session is available — `docs/self-improvement/categories/test/2026-09-10-multi-jira-domain-picker-bucket-e.md`.

## Out of scope (flagged, not designed)

- Multi-site Plane / GitHub / Linear — no-action; same pattern can copy later.
- Per-domain Views workspaces — deferred; shared JQL is the requested "change the domain in Views - Jira".
- Side-by-side two Jira grids — needs pane `backendKey` = `Jira:<host>` in the add-pane picker; follow-up if asked.

## Implementation log

*(populated post-ship)*

## Deviations from plan

*(populated post-ship)*

## Verification (actual)

*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)

1. flip the § Status header to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.

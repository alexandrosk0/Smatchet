# Remove global `projectKey` — multi-project design
<!-- index-summary: Multi-project design — remove the singleton `TrackerConfig::ProjectKey` / Plane equivalent; resolve project per call site (view JQL, selection prefix, explicit picker). -->

Status: design / not yet started
Owner: TBD
Scope option: **(a) full removal** of the global `TrackerConfig::ProjectKey` setting. The app must keep working across multiple projects simultaneously; the active project is derived from context (view JQL, selected ticket, or explicit per-operation argument). No per-view project setting, no auto-derive-then-hide, no UI-only hiding.

---

## 1. Goal

Today `TrackerConfig::ProjectKey` (Jira) and the `PlaneProjectId` / `PlaneWorkspaceSlug` pair (Plane) act as a *singleton* "current project" that backs new-issue defaults, the field-catalog cache key, and several Jira REST URLs. We want to delete `ProjectKey` from `TrackerConfig` entirely and replace its three duties (default for new-issue draft, cache key, REST URL parameter) with *contextual* resolution at each call site:

- Views: project comes from the view's JQL (or "all projects" if the JQL has no `project = …` clause).
- Selection-driven actions (clone, child-of, bulk edit): project comes from the selected ticket key prefix.
- New-issue / create-meta: explicit user choice via a project picker at the point of operation; persisted on the draft, not in `TrackerConfig`.
- Field-catalog cache: keyed by `(backend, endpoint, project)`, with one entry per project the user has touched.

The endpoint identity (`Domain` for Jira, `PlaneUrl + PlaneWorkspaceSlug` for Plane) stays in `TrackerConfig`. **Plane's `PlaneProjectId` is treated the same as Jira's `ProjectKey` and is removed in the same migration** — see §2.5. The "workspace" (Jira tenant / Plane workspace slug) remains a global config; only the per-project scope is deleted.

---

## 2. Affected components

### 2.1 Config (interface delta)

| File | Change shape |
|---|---|
| `Source_Core/include/ConfigManager.h:62` | Remove `std::string ProjectKey;` from `TrackerConfig`. |
| `Source_Core/include/ConfigManager.h` (TrackerConfig, Plane block, L68–71) | Remove `PlaneProjectId`. Keep `PlaneUrl` and `PlaneWorkspaceSlug` (workspace-level, not project). |
| `Source_Core/src/ConfigManager.cpp:817` (`ToJson`) | Drop `j["project_key"] = …`. |
| `Source_Core/src/ConfigManager.cpp:1053` (`FromJson`) | Drop the `project_key` read. Run a one-shot migration (§3.1) that converts the legacy value into a "last-used project" entry so installs upgrading mid-stream don't lose the value silently. |

### 2.2 Preferences UI (UI surface)

| File | Change shape |
|---|---|
| `Source_Core/src/SmatchetPreferencesUi.cpp:111`, `:158`, `:928` | Delete the `projectKeyBuf` field, the `ImGui::InputText("Project Key", …)` row, and the writeback in `Save`. Replace with a read-only "Recently used projects" listbox sourced from the per-project field-catalog cache index (purely informational; no edit). |
| `Source_Core/src/SmatchetLocalization.cpp` (key `prefs.projectKey`) | Mark string deprecated; keep key alive one release for translators. New key: `prefs.recentProjects`. |

### 2.3 New-issue draft surfaces (behavior change)

| File | Change shape |
|---|---|
| `Source_Core/include/IssueDraft.h` | `IssueDraft::ProjectKey` **stays** (per-draft, not global) — this is the canonical place the project now lives. |
| `Source_Core/src/IssueDraft.cpp:107–131` (`FromCachedTicket`) | `fallbackProjectKey` parameter remains *as a parameter*, but callers no longer pass `cfg.ProjectKey`. Replace with: (a) project key parsed from the *last visible ticket's* `id` prefix; (b) if none, project derived from the active view's JQL (§2.4); (c) if none, empty — UI surfaces a picker. |
| `Source_Core/src/SmatchetNewIssueDraftUi.cpp:180,230,358,382` | Already reads/writes `d.newIssueDraft.ProjectKey`. Add an explicit "Project" picker at the top of the draft panel (combobox populated from cache index ∪ "Browse server projects…"). When user changes project, clear / re-resolve issue-type and re-fetch required-fields. Auto-seed the picker from the active-view JQL on draft open. |
| `Source_Core/src/SmatchetGridHeaderUi.cpp:464` | Same — replace `d.cfg.ProjectKey` argument with `ResolveProjectForDraft(activeView, lastVisibleTicket)`. |
| `Source_Core/src/AppController_IssueCreateOffline.cpp:62,70,92` | Same. Existing fallback chain (entry.ProjectKey vs draft.ProjectKey) is fine — it never relied on `cfg`. |

### 2.4 JQL → project resolver (new helper)

New file (or new function inside existing `TrackerHttpUtils` / a new `JqlProjectExtractor.cpp` in `Source_Core/src/`):

```
namespace JqlProjectScope {
    /** Best-effort extract of a single project key from JQL.
     *  Returns "" if none or multiple. Recognizes:
     *    project = PROJ          project in (PROJ)
     *    project = "PROJ"        project = 10001  (returns numeric id; caller resolves)
     *  Does NOT execute the query — pure parse. */
    std::string ExtractSingleProject(const std::string& jql);

    /** True iff the JQL clearly scopes to >=1 project (any of the above forms). */
    bool HasProjectScope(const std::string& jql);
}
```

Callers: new-issue defaults, "create child of …" action, the field-catalog warmup on view switch. **Pure C++14, no regex backreferences** — hand-written scan over the JQL string is fine (tens of LoC).

For Plane, the analogous extractor reads `ProjectId` from the active view's Plane query JSON (Plane queries are structured, not free-form). New helper in `PlaneClient.cpp`.

### 2.5 Plane parity

Today `cfg.PlaneProjectId` is the Plane analog of `cfg.ProjectKey`. It is removed under the same flag. Plane operations need a `(workspace, project)` pair on every request; we thread the project through the same call sites that thread `IssueDraft::ProjectKey`. The "active project" for a Plane view comes from the view's stored query (Plane saves queries as JSON in the same `ViewDefinition.Jql` slot — confirm during PR 1; if not, add a `BackendQuery Jql` carrier).

**Open question OQ-1**: does any current Plane view definition rely on `cfg.PlaneProjectId` as an implicit filter? Audit needed in PR 1.

<!-- PR 1 audit -->
**OQ-1 finding (PR 1, 2026-05-12)**: yes — `cfg.PlaneProjectId` is the **only** project source PlaneClient currently consults. `PlaneClient::FetchIssuesStreamed` (`Source_Core/src/PlaneClient.cpp:380–437`) gates on a non-empty `cfg.PlaneProjectId` and calls `ResolvePlaneProject(...)` against it; every `FetchIssues*` / `FetchIssuesForKeys` / `FetchFieldCatalog` path threads through that resolved id. `PlaneClient.cpp` contains **zero references to `ViewDefinition::Jql`** — the Plane backend ignores per-view query JSON entirely today. Implication for PR 2+: when the global `PlaneProjectId` is removed, every Plane call site must thread a per-operation project UUID (from active-view JQL JSON, last-visible ticket, or explicit picker) into the existing `Resolve*` plumbing. The new `PlaneClient::ExtractProjectFromQuery` helper (PR 1) is the parse side; the threading is PR 2 work. No existing Plane view definition relies on the JQL slot — they all rely on the singleton, which means the migration must dead-letter or auto-fill the Plane equivalent of legacy views (analogous to §2.7 for pending_creates).
<!-- /PR 1 audit -->

### 2.6 Field-catalog cache (keying + storage)

| File | Change shape |
|---|---|
| `Source_Core/src/FieldCatalogCache.cpp:262–268` (`BuildFieldCatalogCacheKey`) | Take an explicit `projectKey` (Jira) / `projectId` (Plane) argument instead of reading from `cfg`. Returns one cache entry **per project**. |
| `Source_Core/src/FieldCatalogCache.cpp:161,175` | `IssueTypeCreateMeta::ProjectKey` JSON field stays. |
| `Source_Core/src/TrackerFieldCatalog.cpp:108,117,322,332,371,382,484,605,614` | All `cfg.ProjectKey` reads become a `const std::string& projectKey` parameter on the relevant fetcher (`FetchCreateMeta`, `FetchSprintsForProject`, etc.). Caller (AppController) supplies the project for the *current operation*. |
| Cache eviction | LRU cap on the number of project entries (e.g. 16). Index file `field_catalog_cache.json` already has a schema_version (currently 2) — bump to 3 and add an `entries: [{cacheKey, lastUsedUnix}]` index. |

### 2.7 Offline queue (schema migration)

`pending_creates` (LocalCacheManager.cpp:51) stores `payload` (an `IssueDraft` JSON blob). The draft already carries `ProjectKey` (`IssueDraft.cpp:218`), so **no SQLite schema change is required** for new entries. But:

- Legacy pending_create rows authored before this change may have an empty `ProjectKey` in their stored draft (when the user relied on the now-removed config fallback). Replay (`OfflineQueueService.cpp:840`) currently passes `draft.ProjectKey` to `GetRequiredFieldSet`. Migration: on first launch of the new build, scan `pending_creates`; for any row whose draft `ProjectKey` is empty, attempt to set it from (a) the legacy `cfg.project_key` value captured during config migration (§3.1) or (b) the parent `ExistingIssueKey` prefix if present; if both fail, **move the row to `pending_creates_dead`** with reason `legacy_missing_project` so the user can re-author it. Logged at WARN with `pending_create_id`.

### 2.8 Saved views

`ViewDefinition.Jql` (ConfigManager.h:185) already carries the JQL string per view, so views *implicitly* gain project scope from JQL. No schema change. But:

- Views authored before this change with no `project = …` clause will, after the change, query *all* projects the user has access to — usually fine for the typical `assignee=currentUser()` default, but a power user with a view like `status = Open` will see a behavior expansion. PR 1 logs a one-time INFO listing views with no project scope.

### 2.9 Commands / CLI / Lua / MCP (public surface)

| File | Change shape |
|---|---|
| `Source_Core/src/Commands/BuiltinCommands.cpp:1035` (`config.get_all`) | Drop `all["projectKey"] = cfg.ProjectKey`. |
| `Source_Core/src/Commands/BuiltinCommands.cpp:1213,1218,1222,1247` (`ticket.create`) | `projectKey` is already a **required** param on `ticket.create` (docs/guides/cli.md:250). No change to the schema — the command was already explicit. Remove the "falls back to `cfg.projectKey`" sentence in the description (it never actually fell back in code but the help text implies it). |
| `Source_Core/src/Commands/BuiltinCommands.cpp:1380` (`{"projectKey", "project_key", ""}` config-set table) | Remove the row — config no longer has this field. |
| `Source_Core/src/AppController_LuaBindings.cpp:254` (`projectkey` / `project_key` case in config setter) | Remove the case; emit a deprecation warning if called. |
| `Plugins/Mcp/` | Confirm no MCP tool currently exposes `config.set project_key`. If `config.get` previously returned `projectKey`, drop the field from the response (wire-format break — call out in release notes). |
| `docs/guides/cli.md:378` | Remove the `projectKey` row from the config table. Add a note: "Project is per-operation; pass `--projectKey` on `ticket.create`, or it's inferred from the active view." |
| `docs/guides/lua.md` | Same removal. |

### 2.10 Bulk import / serializer

| File | Change shape |
|---|---|
| `Source_Core/src/SmatchetBulkTicketsUi.cpp:174,349` | Replace `d.cfg.ProjectKey` (passed as `fallbackProjectKey` to `ParseDrafts`) with `ResolveProjectForBulkImport()` — which uses the active view's JQL project, falling back to a small modal "Bulk import target project" picker shown *before* parse if the view has no project scope. |
| `Source_Core/include/IssueTableSerializer.h:49`, `Source_Core/src/IssueTableSerializer.cpp:191,219,234,269,358,365,367,370` | `fallbackProjectKey` parameter stays — semantics change from "global default" to "what to use for rows that don't carry their own". |

### 2.11 Mutation / pipeline

| File | Change shape |
|---|---|
| `Source_Core/src/JiraIssueMutation.cpp:724,728` | Already reads `draft.ProjectKey`. No change. |
| `Source_Core/src/IssueCreatePipeline.cpp:325` | Already reads `work.ProjectKey`. No change. |

### 2.12 Per-backend leakage check

Per AGENTS.md: backend-specific code stays inside the concrete `ITrackerClient` impl. The new project-resolver helpers (`JqlProjectScope::ExtractSingleProject`, the Plane equivalent) must live in or be called *only from* the concrete client when they parse backend-specific query syntax. The abstract `ITrackerClient` gains **at most**:

```
virtual std::string ExtractProjectFromQuery(const std::string& query) const = 0;
virtual std::vector<RemoteProject> ListProjects(...) = 0;  // for the picker
```

`RemoteProject` is a small POD (`{id, key, displayName}`) defined in `Source_Core/include/TrackerFieldSchema.h` alongside the other catalog DTOs.

---

## 3. Schema migrations

### 3.1 Config JSON (`smatchet.json`)

On `ConfigManager::FromJson`:

1. If `project_key` is present and non-empty, capture it into a transient `legacyProjectKey` for the offline-queue migration (§2.7).
2. Drop the field. Do not error if present (forward-compat with older builds writing it back is fine — we just ignore it).
3. Append a one-time INFO log: `Migrated legacy global project_key='PROJ' — see release notes`.
4. Same handling for `plane_project_id`.

No version bump on the config file itself (the field's absence is the marker; the additive cache index in §2.6 takes the version bump).

### 3.2 Field-catalog cache JSON

Bump `schema_version` from 2 to 3. Loader at `FieldCatalogCache.cpp:350`:

- v2 → v3: existing single-project entry stays valid (cache key is already opaque). Add an empty `entries` array; populate as entries are touched.
- v1 → v3: existing v1→v2 path runs first; then v2→v3.

### 3.3 SQLite (`LocalCacheManager`)

No table additions. The `pending_creates.payload` blob continues to be an opaque `IssueDraft` JSON. The legacy-row sweep described in §2.7 runs once at startup (gated by a `cache_meta` key `legacy_project_swept = 1`).

### 3.4 Saved views (`views.json`)

No schema change — `ViewDefinition.Jql` already carries query text. One-time INFO log lists views with no project scope (§2.8).

---

## 4. UI implications

- **Removed**: Preferences → Account → "Project Key" InputText row.
- **Added**:
  - Preferences → Account → "Recently used projects" — read-only list (informational), sourced from the field-catalog cache index. Entry-level "Forget" button evicts that project's cache entry.
  - New-Issue draft panel header: explicit "Project" combobox (mandatory; draft cannot submit empty). Seeded from active-view JQL or last-visible-ticket prefix.
  - Active-view chrome (`SmatchetActiveProjectGridUi`): subtle "Project: PROJ" pill near the JQL bar when `JqlProjectScope::ExtractSingleProject` returns a single project; "Project: multi" otherwise. Click to filter the current view to one project.
  - Offline-queue UI (`SmatchetOfflineQueueUi.cpp:181–232`): the rendered "project" column now always reflects `parsed.ProjectKey` from the queued draft (today it already does — confirm). Add a "missing project" badge for rows that got swept to dead-letter in §2.7.
- **Bulk import**: small modal "Choose target project" if the active view has no project scope.

---

## 5. Risks

| ID | Risk | Mitigation |
|---|---|---|
| R-1 | **Field-catalog cache size growth** — N projects × current per-project payload. | LRU cap (default 16 projects). Eviction logged at INFO. Config knob `FieldCatalogCacheMaxProjects`. |
| R-2 | **JQL with no project clause** — views silently broaden from "your project" to "all projects you can read", risking expensive Jira queries. | One-time INFO listing affected views at first launch. New-issue picker forces explicit project. Add JQL warning chip "no project scope" in views dashboard. |
| R-3 | **Offline replay where queued project no longer exists** — user uninstalled the project / permissions revoked. | Existing retry/dead-letter path handles 4xx (`pending_creates_dead`); no new mechanism needed. Logged at WARN with `pending_create_id`. |
| R-4 | **Dual-backend (Jira + Plane) divergence** — Plane's "project" is a UUID, Jira's is a key string. | Keep `IssueDraft::ProjectKey` as `std::string` (already is); document that for Plane it holds the UUID. Backend-specific parsing stays inside the concrete `ITrackerClient`. |
| R-5 | **MCP / CLI wire-format break** — `config.get` no longer returns `projectKey`; downstream scripts that read it break silently. | Release-notes call-out. `config.get` continues to return the field as an empty string for one minor version, then removed. (Soft-deprecate, not hard.) |
| R-6 | **Lua scripts in the wild** that do `app.config.set("project_key", …)`. | The Lua setter still accepts the key for one release; logs a `deprecation` warning each call. After that, hard error. Sync `AppController_LuaBindings.cpp` ↔ `AppController_LuaStubs.cpp` (per AGENTS.md). |
| R-7 | **Save-format compat** — older Smatchet builds reading the new config see the field missing, default to empty; behavior matches the new code path (no global project). | Acceptable; documented as a one-way migration. |
| R-8 | **Dual-target compile** — `Source_Core/` is built into both Standalone (with MCP) and `SmatchetCore_DX12` (without). New picker UI must live in `Source_Core/` and respect `SMATCHET_EMBEDDED_IN_UNREAL` (Plane workspace browsing might need the host's HTTP stack on Unreal). Verify with `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`. |
| R-9 | **Localization key churn** — `prefs.projectKey` removed, `prefs.recentProjects` added, draft picker labels added. | Keep `prefs.projectKey` in `Locales/*.json` for one release marked deprecated to let translators catch up. |
| R-10 | **Bulk-edit selection across projects** — today implicitly impossible (one project). After change, selection may span. | `SmatchetBulkTicketsUi` rejects selections spanning multiple projects for any operation that hits create-meta; allows for field edits that aren't project-scoped (summary, description, custom-field-by-id-that-exists-in-both). Reject is a hard error with the offending keys listed. |

---

## 6. Open questions

1. **OQ-1** (Plane parity): does any current Plane view definition rely on `cfg.PlaneProjectId` as an implicit filter when the saved query has no project clause? Audit needed in PR 1 before removing the field.
2. **OQ-2** (project picker source): for the new-issue picker, do we (a) list only projects the cache has touched, (b) list all projects from `ITrackerClient::ListProjects()`, or (c) hybrid (cache-first, "Browse all…" on demand)? Recommend (c). User input?
3. **OQ-3** (cache cap): is 16 projects the right LRU cap, or should it be unbounded with a size-based cap on the JSON file? Recommend 16 unless a heavy user surfaces during dogfood.
4. **OQ-4** (legacy dead-letter): should the legacy-pending_create sweep (§2.7) dead-letter rows with empty project, or attempt a UI prompt to repair them? Recommend dead-letter (simpler, recoverable from the offline-queue UI's restore action).
5. **OQ-5** (JQL multi-project): when JQL says `project in (PROJ, OTHER)`, do we treat "project context" as "multi" (force explicit picker on every per-project operation), or pick the first? Recommend "multi" → picker.
6. **OQ-6** (subtasks & cross-project parents): can a Jira subtask's parent be in a different project? If yes, the parent-derived project for `ExistingIssueKey` needs to come from the parent's own project, not the subtask's.

---

## 7. Phased rollout

Each PR leaves the app shipping-green. Order:

### PR 1 — Audit + helpers (additive, no behavior change)
- Add `JqlProjectScope::ExtractSingleProject` + Plane equivalent + tests.
- Add `ITrackerClient::ExtractProjectFromQuery` + `ListProjects` (default impl returns empty for now).
- Add `RemoteProject` POD to `TrackerFieldSchema.h`.
- Audit OQ-1 (Plane); record findings in this doc.
- Log one-time INFO listing views without project scope.

### PR 2 — Thread project through call sites (additive)
- `FieldCatalogCache::BuildFieldCatalogCacheKey` gains an explicit project parameter; callers pass `cfg.ProjectKey` (no behavior change yet, but the signature is the new one).
- `TrackerFieldCatalog.cpp` fetchers accept `const std::string& projectKey` parameter; callers pass `cfg.ProjectKey`.
- `IssueDraftHelpers::FromCachedTicket` callers in `SmatchetGridHeaderUi`, `SmatchetNewIssueDraftUi`, `AppController_IssueCreateOffline`, `SmatchetBulkTicketsUi` switch to `ResolveProjectForDraft(activeView, lastVisibleTicket, cfg.ProjectKey /* legacy fallback */)`.

### PR 3 — Cache becomes multi-project
- Bump `field_catalog_cache.json` schema_version 2 → 3. LRU cap of 16. Eviction logged.
- New-issue draft fetches per-project create-meta on project change.

### PR 4 — Project picker UI
- New-issue draft panel gets the "Project" combobox. Seeded from active-view JQL / last ticket / legacy `cfg.ProjectKey` (in that order).
- Active-view chrome shows the "Project: PROJ" pill.
- Bulk import gets the modal picker for unscoped views.

### PR 5 — Offline-queue legacy sweep
- One-shot sweep at startup migrates / dead-letters legacy pending_create rows with empty project.
- `cache_meta.legacy_project_swept = 1` marker.

### PR 6 — Remove the global field
- Delete `TrackerConfig::ProjectKey` and `PlaneProjectId`.
- Delete Preferences row + add "Recently used projects" listbox.
- Drop `config.get_all` field, drop the `config.set` table row, drop the Lua case (with one-release deprecation warning).
- `docs/guides/cli.md` + `docs/guides/lua.md` updates.
- Locale strings: `prefs.projectKey` marked deprecated, `prefs.recentProjects` added.
- Full-verify build: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`.

### PR 7 (optional, ~1 release later) — Hard removal of deprecations
- Locale string `prefs.projectKey` deleted.
- Lua deprecation warning becomes hard error.
- `config.get` stops returning `projectKey` even as empty string.

---

## 8. Inventory cross-reference

Files touching `ProjectKey` / `project_key` / `projectKey` today (26 hits, grouped):

- **Config**: `ConfigManager.{h:62, cpp:817,1053}`, `SmatchetPreferencesUi.cpp:111,158,928`, `SmatchetLocalization.cpp` (`prefs.projectKey`).
- **Issue draft / create**: `IssueDraft.{h:25,67,105, cpp:15,107,131,218,247}`, `IssueCreatePipeline.cpp:325`, `JiraIssueMutation.cpp:724,728`, `AppController_IssueCreateOffline.cpp:62,70,92`, `SmatchetNewIssueDraftUi.cpp:180,230,358,382,383`, `SmatchetGridHeaderUi.cpp:464`.
- **Field catalog**: `FieldCatalogCache.{cpp:161,175,268}`, `TrackerFieldCatalog.cpp:108,117,322,332,342,344,371,382,484,605,614`, `TrackerFieldSchema.h:73` (DTO — stays).
- **Bulk / serializer**: `IssueTableSerializer.{h:49, cpp:83,191,219,234,269,358,365,367,370}`, `SmatchetBulkTicketsUi.cpp:174,349`.
- **Offline**: `OfflineQueueService.cpp:840`, `SmatchetOfflineQueueUi.cpp:181,182,231,232`.
- **Commands / Lua / docs**: `BuiltinCommands.cpp:1035,1213,1218,1222,1247,1380`, `AppController_LuaBindings.cpp:254`, `docs/guides/cli.md:250,261,378`, `docs/guides/lua.md`.
- **State carriers (no code change needed; values pass through)**: `NavigationHistory.h`, `SmatchetUiSession.h`, `AppController.h`.

---

## Self-improvement

- `vexp.run_pipeline` returned only generic pivots for a "find every read/write of X" query — 5 pivots, all already known. Falling back to `Grep ProjectKey` produced the actual inventory in one call. Suggestion (process / tooling): for symbol-occurrence-inventory tasks, the agent prompt could nudge straight to `Grep` rather than `run_pipeline`, since vexp is graph-ranked semantic search, not occurrence search. **Mentioned previously?** Worth flagging — if surfaced again it crosses the ≥2-mention threshold for AGENTS.md update.
- `mcp__vexp__run_pipeline` rejected `max_tokens: 14000` as "floating point, expected usize". JSON numbers are always doubles in the wire format; the schema should accept integers-as-floats or the error message should hint at the integer constraint. Minor (tooling).
- The architect prompt forbids writing `.md` files; the user explicitly asked for one. Resolved by user-override priority but worth a tightening: prompt could say "unless the user explicitly requests a design doc deliverable at a specific path". Minor (process).

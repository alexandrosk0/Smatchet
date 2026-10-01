# Plan — namespace the local cache by tracker site and account
<!-- plan-date: 2026-09-30 -->

> **Slug**: `cache-site-identity` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — one PR. Fixes #2268. A Pillar 6 follow-up to [offline-first](../offline-first.md).

## Context

Every row of the local cache carries a `backend_key` that names the tracker it belongs to. The
rows include:
- cached tickets (`tickets_v2` and its two field-value tables);
- saved lookups (`lookup_cache`);
- the three offline write queues (`pending_field_edits`, `pending_creates`, `pending_actions`) and
  their `_dead` tables.

`smatchet::jira_backends::TrackerCacheBackendKey` derives that key. Today it returns:
- `"Jira"` for the first Jira site, whatever its host or account;
- `"Jira:<host>"` for an extra Jira site;
- just the kind (`"Plane"`, `"GitHub"`, `"Linear"`) for every other tracker.

So a same-kind identity change keeps the previous site's data. That covers another Jira host or
account, another Plane workspace, another GitHub repo and another Linear team:
- **Reads.** Offline, or after a failed refresh, the app shows the old site's saved projects,
  users, components, workflow transitions, edit permissions and tickets.
- **Writes.** Queued offline writes replay against the new site, because replay matches rows on
  `backend_key` equality (`OfflineQueueService::FilterRowsToReplayBackendKey`,
  `PendingActionQueueService::IsReplayable`). A comment queued for `OFF-1` on site A can be
  posted to a different `OFF-1` on site B.

The replay rule itself is right. ADR-0018 decision 4 says a row whose key has no live context
"stays queued — never replayed against a wrong backend, never dropped". What is wrong is the key:
it names a kind, not a site.

After this lands, cached data and queued writes belong to one site and account. A queued write is
only ever sent to the site it was written for.

## Approach

1. **Key format: `<Kind>@<site>`. It is readable, contains no secrets or email, and is stable.**

   | Tracker | Key | Identity parts |
   |---|---|---|
   | Jira | `Jira@<host>#<acct>` | live host (`NormalizeJiraHost`) + first 12 hex of `Sha256Hex(lower(trim(email)))` |
   | Plane | `Plane@<host>/<workspace>` | `PlaneUrl` host + lower-cased workspace slug |
   | GitHub | `GitHub@<host>/<owner>/<repo>` | `GitHubBaseUrl` host (default `api.github.com`) + lower-cased owner/repo |
   | Linear | `Linear@<host>/<team>` | `LinearBaseUrl` host + `LinearTeamId` (else `LinearTeamKey`) |

   - **Account.** The account is part of the Jira identity because a queued write replayed under
     another account is attributed to the wrong person. The email is hashed, never stored raw:
     keys appear in logs (`LOG_INFO … backend_key=%s`).
   - **Tokens and API keys** never take part.
   - **Fallback.** When a tracker's identity parts are empty (unconfigured, or a fixture backend
     in tests), the key falls back to the bare kind (`"Jira"`, …). An unconfigured tracker keeps
     today's key, and every fixture-driven test keeps working.
2. **One-time re-key at startup.** A pure `LegacyCacheKeyRekeys(cfg)` maps each legacy key to the
   identity the current config gives it:
   - `"Jira"` → the first instance;
   - each `"Jira:<host>"` → that extra instance, with its email or the inherited first email;
   - `"Plane"`, `"GitHub"`, `"Linear"` → their configured identity.

   A pair is emitted only when the identity is derivable and differs from the legacy key.
   `LocalCacheManager::RunOneTimeCacheIdentityRekey(pairs)` applies them in one transaction,
   stamped `cache_identity_rekey_v1` in `cache_meta`:
   - **Cache tables** (`tickets_v2`, `ticket_field_values_v2`, `ticket_field_rich_values_v2`,
     `lookup_cache`): `UPDATE OR IGNORE`, then delete what a primary-key collision left under the
     legacy key.
   - **Queue tables** (the six `pending_*` tables): plain `UPDATE`; auto-increment ids never
     collide.

   It runs where the other one-time migrations run, before the first ticket read or replay tick.
3. **Queued writes stay bound to their site.** After an identity change their key matches no live
   context, so replay already holds them, and they replay if the user switches back. The gap is
   visibility: the Offline Queue panel stores each row's key but never shows it.
   - A row whose key matches no live context shows "Held: queued for <site>", with the site label
     from `DescribeCacheBackendKey` (e.g. "Jira · acme.atlassian.net"; never the account hash).
   - Retrying a held row is disabled, with the reason in a tooltip. Discard still works.
4. **An identity change is a backend switch.** Today only a kind change or a Jira host change
   resets in-memory state. Both detectors become "the cache key changed":
   - **`TicketSyncService`** clears the in-memory tickets when the re-stamped key differs, then
     re-hydrates from the new namespace.
   - **`SmatchetUI`** gets a session detector keyed on `FocusedCacheBackendKey()`. It discards the
     not-yet-sent grid edits held in RAM, clears the users and requests a catalog refetch.

     This detector is separate from the views detector, which stays keyed on kind: views are
     per-kind config and must not reset on a site change.
5. **Panes resolve identity on creation.**
   - `EnsurePaneContextLive(paneId, kind)` stamps a new context with the identity key, derived
     once from `ConfigManager::Load()` with `TrackerType = kind`. Stamping the raw kind would
     orphan rows written before the first sync re-stamp.
   - The same-backend catalog seed compares identity keys.
   - Key derivation (including SHA-256) runs at init, at sync start and at pane creation. It never
     runs per frame.

## Files to modify

1. **`Source/Core/include/Config/JiraBackendInstancesPure.h`, `Source/Core/src/Config/JiraBackendInstancesPure.cpp`**
   - identity `TrackerCacheBackendKey`;
   - `LegacyCacheKeyRekeys`;
   - `DescribeCacheBackendKey`;
   - `CacheBackendKeyKind`.
2. **`Source/Core/include/ISyncCache.h`, `Source/Core/include/Persistence/LocalCacheManager.h`, `Source/Core/src/Persistence/LocalCacheManager.cpp`, `tests/support/FakeSyncCache.h`**:
   `RunOneTimeCacheIdentityRekey`.
3. **`Source/Core/src/AppController_Init.cpp`, `Source/Core/src/AppController_LocalCacheDb.cpp`**:
   run the re-key before the first read or replay tick.
4. **`Source/Core/src/Sync/TicketSyncService.cpp`**: a key change counts as a backend swap.
5. **`Source/Core/src/AppController_PaneContexts.cpp`, `Source/Core/include/AppController.h`**:
   - identity key on pane creation;
   - the catalog seed compares identity keys;
   - a live-key query for the queue panel.
6. **`Source/Core/src/Ui/SmatchetUI.cpp`**: session identity-change detector.
7. **`Source/Core/src/Ui/SmatchetOfflineQueueUi*.cpp`, `Source/Core/src/SmatchetLocalization.cpp`**:
   held state + site label.
8. **`Source/Core/src/Sync/AGENTS.md`, `Source/Core/src/Persistence/AGENTS.md`**: the key
   identifies a site and account; the one-time re-key.

## Existing utilities reused

- **`smatchet::jira_backends::NormalizeJiraHost`** (`JiraBackendInstancesPure.cpp`): generic
  URL → host normalization (scheme, path, port, IPv6, trailing dot).
- **`smatchet::hashing::Sha256Hex`** (`Sha256.h`): the account hash.
- **`LocalCacheManager::RunOneTimePendingQueueBackendKeyStamp`**: the `cache_meta` one-time
  migration pattern the re-key copies.
- **The ADR-0018 replay filter** (`FilterRowsToReplayBackendKey`, `IsReplayable`): unchanged; it
  already holds rows for other keys.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — nothing is extracted or split.

## UX Pillar callouts

- **Pillar 6.** Saved data never crosses sites, and a write is never sent to a site it was not
  written for. Held writes stay visible and discardable.
- **Pillars 1 and 2.** Nothing new runs per frame. The re-key runs once, at startup (see the gates
  section).
- **Pillar 3.** The re-key is one transaction. A failure leaves the stamp unset, so it retries next
  start, and is logged, not fatal: the same contract as the existing one-time migrations.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **Steady state.** The queue panel's held check compares against the live-context keys, and it
   runs only while the panel is open.
2. **The re-key.** It runs once, in one transaction, where `RunOneTimeTicketsV2CopyMigration`
   already runs, so it adds no new UI-thread pattern. Its cost is bounded by the number of rows.
3. **Key derivation.** SHA-256 of a short string, at init, sync start and pane creation only.
4. **Pillar 2 scanner.** Run `bash scripts/dev/pillar2-scan.sh` on the touched `.cpp` files.

## Risks / non-goals

- **Site already changed before the upgrade.** The first-run re-key assigns the legacy rows to the
  site configured *now*. If the user changed sites before upgrading, the old site's rows land under
  the new identity. Nothing records which site they came from, so this is accepted; the earlier
  `pending_queue_backend_key_stamp_v1` made the same assumption.
- **Account changes on Plane / GitHub / Linear** are not part of the identity: the only account
  identity there is the API key or PAT (a secret), and resolving the account needs the network.
- **Views** stay per kind.

## Verification

- **`JiraBackendInstancesPure.test.cpp`** (both test lists):
  - the identity key for each tracker, and the bare-kind fallback;
  - case and scheme normalization;
  - no email in any key, and different emails give different keys;
  - an extra instance inherits the first email;
  - `LegacyCacheKeyRekeys` for every legacy key, including unconfigured kinds (no pair);
  - `DescribeCacheBackendKey` and `CacheBackendKeyKind`.
- **`CacheIdentityRekeySqlite.test.cpp`** (SmatchetTests): legacy rows move in every table.
  Collisions keep the destination row. A second run is a no-op. An empty pair list still stamps.
  Queue rows keep their ids and payloads.
- **Queue services**: a row keyed for another identity is neither replayed nor dropped.
- **Bucket E** (`OfflineFirst`), API-level. Queue a comment offline, change the Jira account in
  the live config, and bring the network up: the row is held (not sent). Restore the account and
  it replays exactly once.
- **Gates**: pre-ship, lint selftest, `posix-core-check`, Linux doctests; CI for the Windows suites.

## Out of scope (flagged, not designed)

- **GitHub catalog snapshot key.** `FieldCatalogCache::BuildFieldCatalogCacheKey` falls into its
  Jira branch for GitHub. Snapshot keys already carry the endpoint and are otherwise
  site-specific; the GitHub branch is flagged, not fixed here.

## Implementation log

- **Site keys.** `Config/CacheBackendKeyPure.{h,cpp}` replaces `jira_backends::TrackerCacheBackendKey`. It
  provides:
  - `TrackerCacheBackendKey`;
  - `LegacyCacheKeyRekeys`;
  - `CacheBackendKeyKind`;
  - `DescribeCacheBackendKey`;
  - `IsHeldCacheKey`.

  The two callers (`AppController_Init`, `TicketSyncService`) switched over.
- **One-time re-key.** `LocalCacheManager_CacheIdentity.cpp` implements `RunOneTimeCacheIdentityRekey`,
  run at init after the two older stamp migrations, and flag-only after a database rebuild.
- **Backend switch.** `TicketSyncService` tracks `lastAppliedCacheKey_` in place of
  `lastAppliedJiraHost_`. A same-kind key change recreates the client and clears the in-memory tickets.
- **Session reset.** `SmatchetUI::ResetOnCacheSiteChange` discards unsent grid edits, clears the catalog
  and users, and refetches the catalog. The sync latches are left alone.
- **Pane creation.** `EnsurePaneContextLive` stamps new contexts with the site key via
  `ResolvePaneCacheKey`.
- **Held rows.** `AppController::LiveCacheBackendKeys` (also on `IAppPendingActions`) feeds a "Held" state
  and a site tooltip in both Offline Queue tables.
- **Tests:**
  - `CacheBackendKeyPure.test.cpp`;
  - `LocalCacheIdentityRekey.test.cpp`;
  - a same-kind site-change case in `TrackerBackendFactoryConfig.test.cpp`;
  - bucket E `OfflineFirst/Queue_HeldForAnotherSiteIsNeverSent`.

- **CodeRabbit round 1.** The restored-pane snapshot read in `EnsurePaneLiveSyncStarted` (tickets and
  owned ids) still used the bare tracker kind, so a restored pane would not have found its saved rows.
  It now reads under the pane's site key, the key the sync stamps and the owned ids are recorded with.
- **Bucket-E held-rows check.** The first CI run held both rows correctly but never counted them as
  drawn, for two reasons in the test:
  - it read the panel inside the docked Active Project pane, which the dock layout can hide or clip
    (a clipped table draws no rows);
  - its poll re-ran a counter-resetting predicate after success.

  The test now draws the real panel in a test-owned window that fills the work area, and polls the held
  count of that one draw.
- **CodeRabbit round 2.** `RemoveLocalCacheDbFiles` treated a file it could not inspect as already
  removed. "Recreate database" could then reopen the old file as the fresh one and mark its one-time
  migrations (the re-key included) done without moving its rows. An inspection error now fails the
  recreate instead.

## Deviations from plan

- **Module name.** The key logic lives in a new `CacheBackendKeyPure` module rather than
  `JiraBackendInstancesPure`: it is not Jira-specific.
- **Re-key input.** `LegacyCacheKeyRekeys` returns `(from, to)` pairs, which is what the SQLite re-key
  takes, so no conversion type is needed.
- **`TicketSyncService` generalization.** The Jira-only host check became the site-key check for every
  kind, so a Plane workspace or GitHub repo change also rebuilds the client (Plane's key-to-id map is
  per workspace).
- **Held-row UI.** The label is a one-word "Held", to fit the existing State column. A test hook
  (`HeldRowsDrawnForTests`) gives bucket E a signal that does not depend on pixels.
- **API-level bucket-E test.** The planned "change the Jira account in the live config" test became a
  row queued under another site's key. It exercises the same replay filter and panel path without
  mutating the shared fixture config.

## Verification (actual)

- **Local, Linux:**
  - `SmatchetTsanTests`: 817/817 cases pass, including the new key, re-key and site-switch cases;
  - `posix-core-check` compiles every core TU;
  - the bucket-E file passes a syntax check;
  - `pre-ship.sh` passes.
- **CI:** Windows `SmatchetTests`, sanitizers and the `OfflineFirst` bucket-E lane (see the PR).

## Archive (post-ship — DO IN THIS PR, never a follow-up)

Moved to `docs/plans/shipped/` in the shipping PR.

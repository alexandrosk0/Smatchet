# Historical code-review findings

> Findings from sweeping **already-merged** PRs with the
> [`historical-code-review`](../../agents/_shared/skills/historical-code-review/SKILL.md)
> skill + `agents/scripts/core/historical-review-survivors.sh`. Each PR is
> reviewed for **only the lines it introduced that are still alive and untouched
> at `origin/develop`** — code a newer PR already changed/fixed is excluded by
> construction, so nothing here is already-resolved.
>
> Reviewer model: `code-review` (opus/high). Findings are advisory backlog, not
> auto-fixed. User-visible product defects should be elevated to GitHub Issues
> (ADR-0014); the rest is tech-debt. Newest batch on top.

<!-- Batches appended at the top. -->

## Batch 1 — PRs #947–951 (swept 2026-06-07)

Survivor coverage (alive/introduced at origin/develop): #951 610/610 · #950 1/1 ·
#949 ~305/306 · #948 690/690 · #947 281/284. Net: **1 HIGH, 0 other.**

### PR #948 (2e9a7fbf) — multi-grid Slice 1b: tickets_v2 namespacing migration
- **HIGH · still alive** — `Source/Core/src/AppController.cpp` `InitConfig` migration block (~L1214-1220): the one-time tickets_v2 copy migration stamps legacy rows with `NormalizeViewsBackendKey(backendType)` (the `Initialize` **param**), but the live read/write key is re-derived in `InitBackends` (`AppController.cpp:1344`) from `ConfigManager::Load().TrackerType` — which ignores the ephemeral `--backend-type`/`-b` CLI override (`StandaloneAppBootstrap.cpp`) and the embedder's `options.BackendType` (`SmatchetImGuiHost.cpp:636`). Launch e.g. `Smatchet -b Plane` once on a pre-1b DB whose persisted tracker is Jira → migration copies Jira rows into `tickets_v2` under `"Plane"`, **consumes the `cache_meta` flag permanently**, while the live path reads under `"Jira"` (empty): legacy cache stranded + wrong namespace polluted, never re-migrated.
  **Fix:** run the migration with the resolved live key *after* `InitBackends` re-stamps it (`focusedContext().CacheBackendKeyCopy()`), exactly as `RecreateLocalCacheDatabase` already does (`AppController.cpp:2132`). The 1218-1219 comment's "fixtures use fresh DBs" rationale covers only the env-factory case, not this CLI/embedder one.
  **Route:** user-visible (cached tickets vanish on upgrade for non-default backends) → GitHub Issue candidate (ADR-0014). Originally surfaced as CR-948-1 in the live PR review; this confirms it is **still unfixed on origin/develop**.

### PR #947 (7835dba3) — guard-head-drift worktree git -C + PowerShell
- Clean (no NEW bug in the surviving set). CR-947-1 (trailing-boundary regex) already **fixed by #956** → correctly excluded. CR-947-2 (space-in-`-C`-path false-block, fail-safe) already backlogged in `tooling.md`.

### PR #949 (f7411db7) — perf: per-scope p99Ms in snapshots
- Clean. Percentile math (`ceil(0.99n)` rank, in-bounds for all n), ring wraparound/bounds, and cold-path locking all verified correct on the surviving lines. (Some #949 perf lines were re-attributed to the #963 100 Hz change and excluded.)

### PR #950 (fef198e4) — config strict=false
- Clean. Sole survivor `project.config.json` `branch_protection.strict: false` — valid JSON, matches the documented merge-throughput decision; required-contexts lists unchanged.

### PR #951 (258116f2) — multi-grid Slice 1c: pending-queue BackendKey + replay
- Clean. All SQLite column↔bind index orderings verified after the inserted `backend_key`; migration idempotent/transactional/empty-key-guarded; replay filter never replays against a wrong backend nor drops rows. (Earlier live-review CR-951-1 was a *verify* item about the dead-letter restore UI path; the surviving cache path `RestoreDeadPendingCreate` re-queues under the original key correctly.)

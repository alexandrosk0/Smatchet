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

## Batch 2 — PRs #926–946 (swept 2026-06-07)

20 PRs (#946,945,944,942,941,940,939,938,937,936,935,934,933,932,931,930,929,928,927,926). Net: **0 HIGH, 2 MEDIUM, doc-drift LOW cluster.** Fully superseded (nothing alive): #941, #936, #929. Clean: #946, #945 (faithful behaviour-identical extraction — destruction order + ADR-0012 atomics verified), #944, #942, #939, #933, #927, #934.

### MEDIUM
- **#935 (e3996308)** — `docs/agent-rules/merge-gates.md:10` quotes the meant-to-block allow-list regex as `Coverage|Sanitizer|Bucket-`, but live `merge-gates.sh:365` is `Coverage|Sanitizer|Bucket-|Perf PR-fast` (the 4th pattern added later by #942). A reader concludes a red `Perf PR-fast` is non-blocking when it actually blocks — dangerous doc-vs-code drift on a merge-gate. Same omission (LOW) at `merge-gates.sh:9` header comment and `AGENTS.md:49`. **Fix:** append `|Perf PR-fast` (or soften to "e.g.") at all three sites; consider a selftest asserting the regex literal mirrors into the docs.
- **#928 (394746a8)** — `Source/Core/src/Tracker/CONTEXT.md` documents as *live data-model facts* terms that exist nowhere in `Source/` on develop: `:43-47` `TrackerActivityEntry` + `GroupMemberCache`/"group roster" (Slice 2 never landed, gated behind unshipped multi-grid), and `:26` claims `ITrackerCollaboration` "handles ... per-user activity" but the interface has no `FetchUserActivity` (the PR's own plan says no activity endpoint exists). Canonical leaf-doc describes vaporware → readers chase non-existent symbols. **Fix:** mark both as planned/forthcoming or revert the CONTEXT.md additions until the backend slice ships.

### LOW (notable)
- **#940 (af465eb8)** — `docs/adr/0018-multi-grid-pane-contexts.md:6` broken ref `docs/plans/multi-grid-tabs.md` → should be `docs/plans/active/multi-grid-tabs.md`; `:3` status still `proposed` though Slice 1 shipped; stale `AppController.h` line citations in the design addendum.
- **#937 (b5716262)** — `scripts/dev/perf-run.sh:152` inline JSON validator checks `data.rows` then top-level `rows`, inverted vs `perf-compare.py extract_rows()` (top-level first); on a malformed file carrying both, perf-run.sh PASSes while perf-compare reads `[]` → false PASS. Match the order.
- **#931 (27e9e428)** — `postmortems.md:269` "### Filed as … (P1, decision-pending)" stale (resolved option B, #933, per RESOLVED note above it); stale `merge-gates.sh:345` line citation.
- **#932 (5f2dd18b)** — `build-quality-velocity-hardening.md:196` status block lists #8/#13 "Parked" but impl-log (:175) records "UN-PARKED → GATE ARMED"; impl-log missing bullets for #920/#925/#926.
- **#930 (7531a53c)** — `session-guard-agnostic.md:75-78` nested-backtick markdown breaks the Perf-gates `N/A` block rendering.
- **#938 (09f4c791)** — `TicketSyncService.test.cpp:427` mislocated comment; `SpinUntil` 400 ms cap is a latent flake-watch (5 new dependents).
- **#926 (c9f0c9dc)** — `data_dependent_windows_smoke.test.cpp`: stale Views-Dashboard probe comment, `"SMAT-1"` vs `"SMAT-TEST-1"` comment mismatch, and `app==nullptr` logs SKIP but records PASS (vacuous-green if harness fails to boot) — `IM_CHECK(app!=nullptr)` would fail loudly.
- **#942 (3e381a8c)** — informational only: armed relative gate inert on 3/4 scenarios (calls<min_baseline_calls) + warmup-contaminated `emaAvgMs` baked into approved baselines (unused by current gate). Calibration-phase state, not a defect.

### Note on doc-drift recurrence
Several findings (#935, #931, #932, #934, #940) are the same class: a later PR changed code/status and left mirror docs (allow-list, postmortem status, plan status, ADR status) stale. Candidate standing gate: mirror-literal selftests + a "doc status vs shipped" check. (#934 itself flagged the #942-induced AGENTS.md staleness — same root as #935.)

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

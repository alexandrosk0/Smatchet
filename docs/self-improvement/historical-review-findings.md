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

## Batch 3 — PRs #809–925 (100-PR workflow sweep, 2026-06-07)

Ran via the `historical-review-sweep` workflow (100 code-review agents, concurrency-capped, structured output). Coverage: **100 reviewed — 20 with findings, 65 clean, 15 fully superseded.** Net: **5 HIGH, 4 MEDIUM, 14 LOW.**

### HIGH
- **#854 (7d7b2f01) · `Source/Core/src/Sync/OfflineQueueService.cpp:617`** — scalar conflict-resolve writes the chosen DISPLAY string verbatim into the payload key, but every ID/object/array-valued field (single/multi-select, status, priority, issuetype, user, component, cascading, labels) stores an object/array payload (`{"id":"3"}` …). Resolve clobbers `{"priority":{"id":"3"}}` → `{"priority":"Low"}`; `ReplayOneFieldEdit` PUTs it → Jira 400 → retries → dead-letter → **user's resolved offline edit silently lost** (despite "edit re-queued" toast). "Use Mine" equally broken. Test uses a bare-string payload so CI never sees it. **→ Issue candidate (data-loss).** Fix: route scalar "Use Mine" through the unchanged-payload path; for "Use Theirs"/"Save" rebuild via `BuildFieldPayload/BuildValue`; only overwrite with a bare string when the existing value is itself a string. Add an object-payload test.
- **#892 (1e085193) · `Source/Core/src/Ui/SmatchetPreferencesUi.cpp:324`** — `DrawTrackerRecentProjects` calls `FieldCatalogCache::ListCachedProjects()` **every frame** the Prefs→Tracker tab is open → synchronous `ifstream` read + full JSON parse + schema migration + sort on the UI render thread (Pillar-2 violation). Slow/locked/large cache file stalls the window. (Relocated by the refactor, but line 324 is the surviving call.) **→ Issue candidate (UI freeze).** Fix: read+filter once on tab-open, cache in `UiDrawSession`, refetch only on backend change / Forget; or load on a worker.
- **#834 (2fd9ef33) · `scripts/dev/coverage.sh:168`** — OCC exclude patterns use the bare token `Ui` as an unanchored case-insensitive substring → excludes far more than `Ui/`: the whole strict-zone `Source/Core/src/Commands/Builtin/` (`Builtin`), `CommandPaletteUi.cpp`, `Commands/Scenarios/UiTestScenario.cpp`, `AiContextBuilder.cpp` (`Builder`). The **blocking** coverage gate's 67% baseline is computed on a too-small surface; strict-zone code uncounted; tests there can't move the gate. Fix: anchor to a dir boundary (`Source*Core*src\Ui\`), re-verify the captured file list, re-baseline.
- **#918 (c2287c02) · `scripts/dev/coverage-delta-gate.sh:79`** — classifier `'*'*) return 0` (meant for block-comment continuations, already handled by the state machine) instead only fires on real statements starting with deref/indirection: `*out = compute();`, `*it = next();` → classified no-runtime-surface → a diff of output-pointer writes is falsely auto-EXEMPTED from the required test-delta gate (false PASS, unsafe direction; violates the file's own CONSERVATIVE contract). Fix: drop the redundant comment-continuation cases or tighten `'*'*` to a true continuation shape.
- **#919 (1d83a108) · `tests/bats/merge_watcher.bats:354`** — the two new `handle_pass` tests (enqueue-on-queue, merge-when-no-queue — the entire point of #919) FAIL on Windows: (1) the gh stub selector `case "$2 $3"` no longer matches now that #919 moved the discriminator to `$1/$2` (`gh pr merge`/`gh repo view`), falls through to `exit 0`; (2) the `PATH=$STUB_BIN gh` trick is documented non-functional on Windows (native `shutil.which` skips extensionless stubs → real `gh.exe` runs). So merge-queue-safety logic has **zero working coverage**; CI misses it (no bats on the Windows runner). Fix: drive the 3 tests through the `mw.squash_merge_pr`/`_gh_owner_repo` monkeypatch seams; if keeping a stub, name it `gh.cmd` and use `case "$1 $2"`.

### MEDIUM
- **#918 · `coverage-delta-gate.sh:77`** — `'/*'*) return 0` exempts open-and-close comment lines with trailing code (`/* note */ launchTask();`); mirror hole at `:185` (`*/` + code → `continue` drops the statement). Both silently exempt real surface. Fix: strip the comment span, classify the residual code.
- **#814 (c0e4f4e5) · `agents/scripts/project/migrate-bugs-to-issues.sh:12`** — usage header claims `--apply` "create Issues + move debt + prune bug.md" but apply only creates Issues (debt/bug.md reconciliation is manual by design). Fix: correct the header.
- **#813 (f3652350) · `project.config.json:146`** — `governance.loop_mode` is a dead knob: `_doc` (+ AI_POLICY.md, ship-loops.md) claims it's the SessionStart default, but the only consumer `clear-session-context.sh` reads only `SMATCHET_LOOP_MODE` env and hardcodes `in` else. Harmless only because both equal `in`. Fix: read `governance.loop_mode` as the unset-env fallback, or reword the docs to "advisory/unconsumed".
- **#810 (1ec969d5) · `docs/agent-rules/issue-triage.md:68`** — documents the `[issue-propose]` line as `(P<k>, area:X)` but `issue-sweep.sh:111` emits priority only `(P0)`/`(P1)`, no area. Fix: drop `area:X` from the doc or append the area label in the emitter.

### LOW (14)
- **#917** `test-lua-mirror-smoke.sh:58` `mapfile` breaks on macOS bash 3.2 (no summary line → silent gate degrade); not on CI/Win targets.
- **#915** `check-test-list.sh:29` + `tests/CMakeLists.txt:608` — substring (not path-boundary) match → a basename that's a suffix of a referenced file is falsely "referenced" → uncompiled test (the exact false-green this guard prevents).
- **#914** `infra.md:31` `test-delta-test-light-exemption` still `open` but shipped (#918) → mark applied/archive; leave sibling `pr-burst-guard` open.
- **#913** `guard-shared-tree.sh:51` doesn't exempt `-C <worktree>`-targeted git ops (false-deny) unlike the parity helper in `guard-head-drift.sh`; `SMATCHET_ALLOW_SHARED_SWITCH=1` overrides.
- **#911** `test-config-globs.sh:72` process-substitution helper crash isn't caught by `set -e` → loop reads 0 globs → PASS (fails OPEN vs documented fail-CLOSED). Capture into a var + check status.
- **#909** `build.md:31` broken ref `scripts/dev/build_and_run.ps1` → `scripts/dev/local/build_and_run.ps1`.
- **#908** `CMakeLists.txt:664` sol2 RE-RUN CLEANUP group is dead (PRIMARY patch FATALs first on a double-appended tree); harmless (SHA-pin re-fetches fresh) but comment is wrong. Delete the dead group or reorder + fix comment.
- **#888** `docs/harness/pi/README.md:57` overstates "read-only enforced by tool scoping" — read-only agents with `shell` get `bash` (can write). Soften wording.
- **#887** `agents/_shared/workflows/pre-merge-review.js:106` `filter(Boolean)` destroys positional identity → on partial reviewer failure the judge mis-attributes code-review vs security-review punch lists. Reference `reviews[0]/[1]` directly.
- **#872** `followup-due-nudge.sh:186` unguarded `"${warns[@]}"` under `set -u` (bash<4.4) in the due-path; mirror line 195's `:-` guard. Dormant on supported toolchains.
- **#855** `build.md:19` broken refs `scripts/dev/build_and_run.ps1`/`build_standalone.ps1` → under `scripts/dev/local/`.
- **#853** `docs/adr/0016-…md:30` stale line-pin `OfflineQueueService.cpp:788` (write moved to :1008). Reference the symbol instead.
- **#814** `issue-sweep.sh:75` relabel verdict fires on missing-priority but apply only adds `bug` → perpetual no-op RELABEL inflating the acted count; `:7` header lists verdicts (`mirror-then-close`/`flag-stale`) the script never emits.
- (Plus the #810/#813/#814 MEDIUMs above each had adjacent doc-vs-code drift.)

**Recurring classes:** (a) gate scripts failing OPEN / false-exempting (#834/#918/#911/#915) — highest-value, undermines blocking gates; (b) stale doc line-pins + moved-script refs (#909/#855/#853/#914/#810/#813); (c) `set -u`/portability latent shell bugs (#872/#917). The gate-false-pass cluster (#834/#918) is worth prioritising — they let untested/uncovered code merge green.

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

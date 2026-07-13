# Smatchet — `Source/Core` Test-Coverage Gap Map

**Date:** 2026-07-02 · **numbers recomputed 2026-07-13**
**Branch:** `claude/fable-5-codebase-improvements-l90taa` · **2026-07-13 pass:** `claude/test-coverage-gap-map-review-kh77tg`
**Scope:** All first-party `.cpp` translation units under `Source/Core/src/` — **331** TUs / **~103.2K LOC** as of 2026-07-13 (was 304 / ~100.8K on 2026-07-05; the tree grew ~25 TUs of ordinary feature/extraction work in between, plus this pass's 2 new tested `_detail` TUs). Headers, `Source/Plugins`, `Source/Standalone`, `Source/Mobile`, and vendored `ThirdParty/` are out of scope (plugins have their own suites under `tests/Plugins/`).
**Companions:** [`CPP_CODE_AUDIT.md`](CPP_CODE_AUDIT.md) (2026-07-01), [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) (2026-06-26), [`backlog/BACKLOG_CODE_REVIEW.md`](backlog/BACKLOG_CODE_REVIEW.md), [`backlog/MANUAL_TEST_QUEUE.md`](backlog/MANUAL_TEST_QUEUE.md).

## Method (and why TU membership is ground truth here)

The test rig intentionally compiles production TUs **directly into the test executables** — `tests/CMakeLists.txt:316` documents the contract ("each test TU lists the production sources it exercises. No production target links test code."). (One deliberate exception has since been added: the Linux-only `SmatchetCommandsTests` target links the `SmatchetCore_PosixCheck` full-core archive to drive a real `AppController` — see Tier 3. Its TUs are NOT counted as "tested" by this map's membership metric, which remains direct-compilation into `SmatchetTests`/`SmatchetTsanTests`/`tests/{ui,fuzz,Lua}` targets.) That makes membership in `add_executable(SmatchetTests ...)` / `SmatchetTsanTests` / `tests/{ui,fuzz,Lua}/CMakeLists.txt` an exact, zero-false-positive signal for *which production code can be exercised by any automated test at all*. This map diffs that set against the full `Source/Core/src/**/*.cpp` inventory.

Granularity caveat: "TU is compiled into a test target" does not mean "TU is well covered" (e.g. `ConfigManager.cpp` is in `SmatchetTests`, yet `CPP_CODE_AUDIT.md` finding #2 sits in its untested `RouteTrackerEnvCredentials` branch). Line-level truth for the tested half lives in the CI Cobertura XML (`coverage.yml` artifact). This map is about the **other** half — the code no gate can see.

## Headline numbers

| Metric | Value (2026-07-13) | (was 2026-07-05) |
|---|---|---|
| Core TUs compiled into ≥1 test target | **177 / 331 (53%)** | 144 / 304 (47%) |
| Core src LOC compiled into ≥1 test target | **~50.7K / 103.2K (49%)** | ~42.7K / 100.8K (42%) |
| Untested TUs | **154** | 160 |
| Stale CMake test references (phantom TUs) | 0 | 0 |

> The untested-TU backlog finally moved below 160: between 2026-07-05 and 2026-07-13 the Tier-2 backend HTTP shells joined the rig (loopback-fixture suites, `coverage-gap-tier2-backend-shell-fixtures.md`), and the 2026-07-13 pass added `Vcs/GitHubCommits.cpp` (the last Tier-2 shell), 9 facet-based `Commands/Builtin` handler TUs + `CommandRegistry.cpp`/`FuzzyMatch.cpp` into `SmatchetTests`, and two new tested UI `_detail` extractions (`SmatchetFieldIconRender_detail`, `AnnotateAnalysisUi_Modals_detail`). The Tier-1 parent shells (`TrackerGridFieldDisplay.cpp`, `TrackerDateTimeFieldEditor.cpp`, `JqlSuggestEngine.cpp`, `SmatchetMergeWatchNotifyServer.cpp`, `AiPrefsTestConnection.cpp`, …) remain untested by design — the pure logic was extracted out of them.

## The structural blind spot: untested TUs are invisible to every coverage gate

The three coverage gates are all denominator-blind to unlinked code:

1. **Aggregate floor** (`coverage.yml`, blocking, `--threshold 65`): OpenCppCoverage instruments only `SmatchetTests.exe` + `SmatchetLuaTests.exe` (`scripts/dev/coverage.sh:178`) and excludes `Source*Core*src*Ui`. A TU never compiled into those exes contributes **zero rows** to the XML — it doesn't drag the number down, it simply doesn't exist. The "65%" is 65% of the tested ~40%, not of `Source/Core`.
2. **Per-file ≥90% gate** (`coverage-perfile-gate.sh`): pins named units and deliberately fails-open on units absent from the XML. *Ratcheted 2026-07-10/13:* grown from the original 4 (`AiEndpointSanitize`, `AiErrorRedact`, `JqlEscape`, `TrackerHttpPure`) to **11** — the six Tier-1 pure units joined on 2026-07-10 with measured rates, and `JqlSuggestEnginePure` joined on 2026-07-13 after a test top-up lifted it 82% → 99%.
3. **Delta gate** (`coverage-delta-gate.sh`): requires a test-file delta when `Source/Core/src` changes — it stops *new* untested surface but never drains the existing 160-TU backlog.

Additionally, the bucket-C/E scenario lanes (the intended coverage story for the UI draw layer and the 30 `Commands/Scenarios/*` TUs) were **dropped from the blocking check set on 2026-06-15** because the Mesa-GL CI runner couldn't boot the exe (`AGENTS.md` § Merge gates). *Resolved 2026-07-05:* the lanes boot and pass again, and **PR #1619 (`05f1f2f`, "block-on-any-red") retired the curated meant-to-block allow-list entirely** — `agents/scripts/core/merge-gates.sh` now sets `MERGE_GATES_BLOCK_ALLOWLIST_RE="."`, so every red/pending CI check (including the bucket-C/E launch-smoke + lane-integrity teeth) gates the merge. The 28.5K LOC of UI draw code and the pending V-series manual smokes have an automated gate again. (Only the stochastic golden-diff / per-test Mesa *steps* remain step-level advisory by design.)

## Where the 154 untested TUs are (by category)

Snapshot 2026-07-05, with the 2026-07-13 deltas noted per row:

| Category | TUs (07-05) | LOC | Verdict |
|---|---|---|---|
| Real logic gaps (non-UI, non-infra) | 40 | 17.9K | Was **the actionable gap** — the Tier-2 backend shells in it are now tested (see Tier 2) |
| Command registration + handlers (`Commands/`, strict zone) | 24 | 4.6K | Consolidated to 19 handler TUs; **9 now compiled into `SmatchetTests`** (2026-07-13) + registry Dispatch — see Tier 3 for the residue |
| UI draw layer (`Ui/`, ImGui immediate-mode) | 62 | 28.5K | Still 63 TUs / ~28.7K (parents stay untested by design); 12 tested `_detail`/pure UI TUs now exist, +2 this pass |
| Shells whose extracted core IS tested (`*Pure`/`*Helpers`/`*Mapping`/`*_detail`/`*Parse` siblings) | 4 | 2.5K | HTTP orchestration now fixture-tested (Tier 2 closed) |
| Test infrastructure (`Commands/Scenarios/*`, `*FixtureBackend`, fault injector) | 30 | 5.3K | Not a gap — this *is* the harness (33 scenario TUs as of 07-13) |

The codebase has a healthy, consistently-applied pattern: extract pure logic into a `*Pure.cpp` / `*_detail.cpp` sibling and doctest it, leaving a thin I/O shell (e.g. `PlaneActivityFeed.cpp` shell vs tested `PlaneActivityFeedPure.cpp`). The gap list below is, to first order, **the units where that extraction hasn't happened yet**.

## Tier 1 — high-risk uncovered logic (do these first)

> **Campaign status (2026-07-05, PRs #1604 / #1607 / #1609 / #1616):** rows #1–#6 and #8 are DONE — each extracted behind a tested pure seam (`TicketGridDurationSortPure`, `TicketFieldEditorLongTextPure` seed/commit, `TrackerGridFieldDisplayPure`, `TrackerDateTimePure` picker/commit, `JqlSuggestEnginePure` + `PlaneQuerySuggestEnginePure`, `MergeWatchNotifyPure`, `AiPrefsTestConnectionPure`). Rows #7/#9/#10 were assessed and closed without extraction: their decision logic already lives in tested units (`BugReportBody`, `CacheEvictionPolicy`/`IconDimensionsPolicy`/`ImageDimensionsPure`, `FieldEditPipelineService`/`OfflineQueueService`/`IssueDraftHelpers`); the residue is AppController/ImGui glue covered by the Tier-4 service-extraction track. **Campaign closed 2026-07-05 (PR #1617)**; plan doc archived to [`docs/plans/shipped/coverage-gap-tier1-pure-extractions.md`](docs/plans/shipped/coverage-gap-tier1-pure-extractions.md). The per-file ≥90% ratchet additions (sequencing step 6) landed 2026-07-10 (six units, measured rates) and 2026-07-13 (`JqlSuggestEnginePure`, after its top-up).


Ranked by (audit findings × ingress exposure × LOC). Every file below has **zero** automated test compilation today.

| # | TU | LOC | Why it's risky |
|---|---|---|---|
| 1 | `TicketGridModel.cpp` | 495 | 2 findings in `CPP_CODE_AUDIT.md`, incl. **#3 Medium: infinite loop in `ParseDurationToSecondsForSort` (UI freeze on sort)**. `tests/Core/TicketGridModelKeySort.test.cpp` explicitly re-implements against `StringUtil.h` instead of linking this TU ("would pull heavy transitive deps") — the duration parser is uncovered. Extract `TicketGridModelSortPure.cpp` (duration parser + comparators), doctest + add a `tests/fuzz/fuzz_duration_sort.cpp` target. |
| 2 | `TicketFieldEditor_Modal.cpp` | 595 | Site of the audit's **#1 High: 64 KiB long-text truncation → server-side data loss**. The `TicketFieldEditorLongTextPure` sibling is tested, but the buffer seed/copy/commit-diff logic (`:460`, `:351`, `:237`, `:492`) lives in the untested modal TU. The fix PR for finding #1 should move that logic into the Pure sibling with regression tests. |
| 3 | `Tracker/TrackerGridFieldDisplay.cpp` | 1003 | **3 `SECURITY_AUDIT.md` findings** — formats tracker-supplied (untrusted) values for every grid cell. Largest single untested non-UI TU. |
| 4 | `Tracker/TrackerDateTimeFieldEditor.cpp` | 694 | Hand-written date/time parsing of user + tracker input (`TrackerDateTimePure` is tested; the editor TU is not). Same defect class as the duration-parser freeze. |
| 5 | `Tracker/JqlSuggestEngine.cpp` + `Tracker/PlaneQuerySuggestEngine.cpp` | 552 + 202 | Tokenizers/parsers over live user keystrokes; only `TrackerQuerySuggestCommon.cpp` is tested. Parser-on-every-keystroke is the highest-frequency untrusted-input path in the app. |
| 6 | `SmatchetMergeWatchNotifyServer.cpp` | 220 | A **network listener** (flagged once in `SECURITY_AUDIT.md`). Ingress without tests. |
| 7 | `Diagnostics/BugReportService.cpp` | 632 | Audit-flagged; assembles + ships diagnostics (redaction correctness matters — `TextRedaction.cpp` is tested, this orchestrator is not). |
| 8 | `AiPrefsTestConnection.cpp` | 241 | Network probe with credential handling; sibling of the gate-pinned `AiEndpointSanitize`. |
| 9 | `Persistence/SmatchetImageTextureCache.cpp` | 344 | Persistence + untrusted image dimensions (`ImageDimensionsPure` tested, cache/eviction TU not). Strict zone (`Persistence/`). |
| 10 | `SmatchetGridFieldEditPipeline.cpp` + `GridContextDepsAdapter.cpp` + `AppController_IssueCreateOffline.cpp` | 266 + 299 + 286 | Field-edit write path (the audit's data-loss class) and offline-create replay; `FieldEditPipelineService.cpp` is tested, these adapters aren't. |

## Tier 2 — backend client HTTP shells (~3.4K LOC) — **CLOSED 2026-07-13**

`GitHubClient.cpp` (673), `GitHubIssueSearch.cpp` (631), `PlaneIssueSearch.cpp` (708, audit-flagged), `PlaneIssueMutation.cpp` (570), `LinearClient.cpp` (556), `LinearIssueSearch.cpp` (374, security-flagged), `LinearIssueMutation.cpp` (239), `PlaneClient.cpp` (158), `Vcs/GitHubCommits.cpp` (96).

*Status:* all nine shells are now compiled into `SmatchetTests` and driven at the in-process httplib loopback (`coverage-gap-tier2-backend-shell-fixtures.md` Slices 1–3 landed the Plane/Linear/GitHub suites — `PlaneIssueSearchHttp` / `LinearIssueSearchHttp` / `GitHubIssueSearchHttp` / `GitHubClientHttp` / `PlaneIssueMutationHttp` / `LinearIssueMutationHttp`; the 2026-07-13 pass added `GitHubCommitsHttp.test.cpp` for the last one, `Vcs/GitHubCommits.cpp`). The mapping/JQL-translation halves were already covered (`*MappingPure`, `*QueryFromJql`, `GitHubFetchPlan`, `GitHubIssueSearchMapping`, `GitHubClientHelpers`, `GitHubCommitsParse`); the fixture suites pin the previously-untested half — pagination, retry/error classification, and request orchestration — which was the point of sequencing them ahead of the **B2** `TrackerHttpClient` batches.

## Tier 3 — Commands strict zone (19 handler TUs after consolidation)

`Commands/` is a strict lint zone and the single registry feeding four frontends (CLI, palette, MCP, Lua). Three layers of coverage now exist:

1. *2026-07-05, PR #1618:* `Commands/Scenarios/CommandContractSweepScenario.cpp` registers every builtin against the fixture backend and asserts the error-envelope contract (scenario lane).
2. *Post-07-05:* the Linux-only `SmatchetCommandsTests` target (+ `SmatchetMonkeyCli` seeded fuzzer) links the `SmatchetCore_PosixCheck` full-core archive, constructs a real headless `AppController`, registers ALL builtins, and drives `CommandRegistry::Dispatch` through the pre-handler pipeline (runs in the mobile-posix-core-check CI lane).
3. *2026-07-13 pass:* the 9 facet-based handler TUs (`_Helpers`, `_App`, `_Tickets`, `_TicketMutations`, `_Sync`, `_Users`, `_Offline`, `_Attach`, `_Automation`) plus `CommandRegistry.cpp`/`FuzzyMatch.cpp` are now compiled **directly into `SmatchetTests`** with fake `IApp*` facets (`tests/Commands/BuiltinFacetCommands.test.cpp`), driving the handler BODIES through real Dispatch — confirm gate, validation, aliases — and putting them in the OpenCppCoverage denominator.

Residue: the 10 handler TUs coupled to `AppController&` or the UI session (`_Ai`, `_Config`, `_Fields`, `_Meta`, `_Perf`, `_BugReport`, `_Debug`, `_Scenario`, `_UiTest`, `_Ui`) plus the `BuiltinCommands.cpp` dispatcher stay out of `SmatchetTests` — they are covered by layers 1–2 only. Migrating them follows the fan-in Phase-5 facet track: each TU that moves off `AppController&` onto a narrow facet becomes harness-eligible for free.

## Tier 4 — AppController family (~7.2K LOC)

`AppController.cpp` (2835), `AppController_LuaBindings.cpp` (1528), `_LuaBindings_Draw.cpp` (1294), `_CatalogAndFieldEdit.cpp` (1124) + small satellites. Don't try to unit-test the monolith — the backlog already has the right plan: keep extracting friend-coupled services behind interfaces (**B1**, **N4**, **N8**; `TicketSyncService`/`OfflineQueueService`/`FieldEditPipelineService`/`AppController_LuaBindingsCore` are extracted *and tested*, proving the loop works). Each extraction PR should carry its golden tests per the B1 note ("testless extraction = lottery").

## Tier 5 — UI draw layer (62 TUs, 28.5K LOC)

Excluded from ctest coverage by explicit policy (`coverage.sh` excludes `Source*Core*src*Ui`; coverage-threshold-graduation Slice 1). The sanctioned mechanisms are (a) the `_detail.cpp` extraction pattern — 10 tested `_detail`/pure UI TUs already exist (`SmatchetGridHeaderUi_detail`, `SmatchetAutocompleteUi_detail`, `AnnotateAnalysisUi_Window_detail`, …) — and (b) the bucket-C/E scenario lanes, **blocking again as of 2026-07-05** (PR #1619 block-on-any-red; the Mesa-GL boot failure is resolved). Two consequences:

- The bucket-C/E lane is bootable and gating merges again, re-arming the scenario TUs + 5.3K LOC of existing harness and closing the Pillar-4 visual-validation gap that previously fell to humans. (Historically this section's highest-leverage action was *restoring* that lane — now done.) The residual UI-coverage work is continued `_detail` extraction to pull draw-layer logic into the ctest-instrumented denominator.
- ~~Prefer `_detail` extraction whenever a UI TU is touched; `SmatchetFieldIconRender.cpp` (737 LOC, 2 audit findings — both now fixed, see `CPP_CODE_AUDIT.md` #14/#22) and `AnnotateAnalysisUi_Modals.cpp` (428, audit-flagged #23, fixed) are the two UI TUs with recently-fixed defects and no extracted core.~~ **Done 2026-07-13** — both extractions landed: `SmatchetFieldIconRender_detail.cpp` (priority-JSON parse, slug normalisation, and the audit-#14 same-origin `iconUrl` confinement) and `AnnotateAnalysisUi_Modals_detail.cpp` (CSV/TSV escaping, quick-comment template expansion, P4-user → Jira-account selection), each with a doctest suite in `SmatchetTests`. The `_detail` extraction preference stands for future UI work — 12 tested `_detail`/pure UI TUs now exist.

## Hygiene notes

- ~~`Source/Core/src/Test_JqlProjectScope.cpp` (49 LOC) is a pre-harness relic … Delete it.~~ **Done 2026-07-05** — the file no longer exists; `tests/Core/JqlProjectScope.test.cpp` is the surviving coverage.
- Backlog reconciliation: `BACKLOG_CODE_REVIEW.md` **N11** ("no tests/ directory exists", "single highest-leverage open item") is fully stale — hundreds of test files across 8 suites now exist; every N11 candidate unit (FuzzyMatch, MarkdownConvert, JqlProjectScope, TextMerge, CompactDateFormat) is covered. **N7** (MarkdownConvert golden tests) is likewise closed (`MarkdownConvert.test.cpp` + `fuzz_markdown_adf`). *Both entries were flipped to ✅ DONE in the backlog — done.*

## Recommended sequencing

1. ~~**Pair with the audit remediation** — findings #1 (long-text truncation) and #3 (duration-sort loop) land in Tier-1 TUs; fix + Pure-extraction + regression test in the same PR each.~~ **Done 2026-07-05** — both fixed (PR #1593) and pinned behind pure seams (`TicketFieldEditorLongTextPure`, `TicketGridDurationSortPure`, PR #1604).
2. ~~**`TrackerGridFieldDisplay` + `TrackerDateTimeFieldEditor` extraction tests** (Tier 1 #3–#4) — the two big untested untrusted-input formatters/parsers (2 PRs).~~ **Done 2026-07-05** — landed as part of the Tier-1 campaign (rows #3–#4: `TrackerGridFieldDisplayPure`, `TrackerDateTimePure`).
3. ~~**Backend-shell fixture tests ahead of each B2 batch** (Tier 2) — sequence with the existing backlog plan rather than as standalone work.~~ **Done 2026-07-13** — all nine shells fixture-tested (the last, `Vcs/GitHubCommits.cpp`, in the 2026-07-13 pass); B2 itself closed 2026-07-11.
4. ~~**One command-invocation harness PR** (Tier 3).~~ **Done** in three layers (scenario sweep #1618; Linux `SmatchetCommandsTests` dispatch harness; 2026-07-13 facet-handler suite in `SmatchetTests`). Residual: the 10 AppController/UI-session-coupled handler TUs (see Tier 3).
5. ~~**Bucket-C/E lane restoration** (Tier 5) — infra work, biggest single unlock.~~ **Done 2026-07-05** — lanes boot and gate merges again (PR #1619 block-on-any-red). Residual: continued `_detail` extraction to move UI draw logic into the ctest denominator.
6. ~~Opportunistic: add each newly-tested trust-boundary unit to the `coverage-perfile-gate.sh` `HIGH_RISK_UNITS` ratchet so it can't rot back.~~ **Done 2026-07-10/13** — the gate pins 11 units; the six Tier-1 pure units joined 2026-07-10, `JqlSuggestEnginePure` joined 2026-07-13 with its test top-up (82% → 99%) in the same change, per the gate's contract ("add a unit only with a test that brings it to ≥90% in the SAME change").

## Prior-state corrections recorded

None of this map's numbers contradict the prior audits; it complements them by measuring *testability reach* rather than defect presence. The one factual correction to circulating docs is the stale N11/N7 state noted above.

# Smatchet — `Source/Core` Test-Coverage Gap Map

**Date:** 2026-07-02 · **numbers recomputed 2026-07-05**
**Branch:** `claude/fable-5-codebase-improvements-l90taa`
**Scope:** All first-party `.cpp` translation units under `Source/Core/src/` — **304** TUs / **~100.8K LOC** as of 2026-07-05 (was 297 / ~99.3K on 2026-07-02; the +7 are the new Tier-1 pure-seam TUs, all immediately tested). Headers, `Source/Plugins`, `Source/Standalone`, `Source/Mobile`, and vendored `ThirdParty/` are out of scope (plugins have their own suites under `tests/Plugins/`).
**Companions:** [`CPP_CODE_AUDIT.md`](CPP_CODE_AUDIT.md) (2026-07-01), [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) (2026-06-26), [`backlog/BACKLOG_CODE_REVIEW.md`](backlog/BACKLOG_CODE_REVIEW.md), [`backlog/MANUAL_TEST_QUEUE.md`](backlog/MANUAL_TEST_QUEUE.md).

## Method (and why TU membership is ground truth here)

The test rig intentionally compiles production TUs **directly into the test executables** — `tests/CMakeLists.txt:272` documents the contract ("each test TU lists the production sources it exercises. No production target links test code."). That makes membership in `add_executable(SmatchetTests ...)` / `SmatchetTsanTests` / `tests/{ui,fuzz,Lua}/CMakeLists.txt` an exact, zero-false-positive signal for *which production code can be exercised by any automated test at all*. This map diffs that set against the full `Source/Core/src/**/*.cpp` inventory.

Granularity caveat: "TU is compiled into a test target" does not mean "TU is well covered" (e.g. `ConfigManager.cpp` is in `SmatchetTests`, yet `CPP_CODE_AUDIT.md` finding #2 sits in its untested `RouteTrackerEnvCredentials` branch). Line-level truth for the tested half lives in the CI Cobertura XML (`coverage.yml` artifact). This map is about the **other** half — the code no gate can see.

## Headline numbers

| Metric | Value (2026-07-05) | (was 2026-07-02) |
|---|---|---|
| Core TUs compiled into ≥1 test target | **144 / 304 (47%)** | 137 / 297 (46%) |
| Core src LOC compiled into ≥1 test target | **~42.7K / 100.8K (42%)** | 40.4K / 99.3K (41%) |
| Untested TUs | **160** | 160 |
| Stale CMake test references (phantom TUs) | 0 | 0 |

> The untested-TU backlog is still **160**: the 7 new Tier-1 seam TUs are both new TUs and immediately tested, so numerator and denominator each moved +7 while the untested set held. Their parent shells (`TrackerGridFieldDisplay.cpp`, `TrackerDateTimeFieldEditor.cpp`, `JqlSuggestEngine.cpp`, `SmatchetMergeWatchNotifyServer.cpp`, `AiPrefsTestConnection.cpp`, …) remain untested by design — the pure logic was extracted out of them.

## The structural blind spot: untested TUs are invisible to every coverage gate

The three coverage gates are all denominator-blind to unlinked code:

1. **Aggregate floor** (`coverage.yml`, blocking, `--threshold 65`): OpenCppCoverage instruments only `SmatchetTests.exe` + `SmatchetLuaTests.exe` (`scripts/dev/coverage.sh:178`) and excludes `Source*Core*src*Ui`. A TU never compiled into those exes contributes **zero rows** to the XML — it doesn't drag the number down, it simply doesn't exist. The "65%" is 65% of the tested ~40%, not of `Source/Core`.
2. **Per-file ≥90% gate** (`coverage-perfile-gate.sh`): pins exactly 4 named units (`AiEndpointSanitize`, `AiErrorRedact`, `JqlEscape`, `TrackerHttpPure`) and deliberately fails-open on units absent from the XML.
3. **Delta gate** (`coverage-delta-gate.sh`): requires a test-file delta when `Source/Core/src` changes — it stops *new* untested surface but never drains the existing 160-TU backlog.

Additionally, the bucket-C/E scenario lanes (the intended coverage story for the UI draw layer and the 30 `Commands/Scenarios/*` TUs) were **dropped from the blocking check set on 2026-06-15** because the Mesa-GL CI runner couldn't boot the exe (`AGENTS.md` § Merge gates). *Resolved 2026-07-05:* the lanes boot and pass again, and **PR #1619 (`05f1f2f`, "block-on-any-red") retired the curated meant-to-block allow-list entirely** — `agents/scripts/core/merge-gates.sh` now sets `MERGE_GATES_BLOCK_ALLOWLIST_RE="."`, so every red/pending CI check (including the bucket-C/E launch-smoke + lane-integrity teeth) gates the merge. The 28.5K LOC of UI draw code and the pending V-series manual smokes have an automated gate again. (Only the stochastic golden-diff / per-test Mesa *steps* remain step-level advisory by design.)

## Where the 160 untested TUs are (by category)

| Category | TUs | LOC | Verdict |
|---|---|---|---|
| Real logic gaps (non-UI, non-infra) | 40 | 17.9K | **The actionable gap** — detail below |
| Command registration + handlers (`Commands/`, strict zone) | 24 | 4.6K | Gap; needs an invocation harness |
| UI draw layer (`Ui/`, ImGui immediate-mode) | 62 | 28.5K | By-design excluded from ctest coverage; gated on bucket-C/E restoration + continued `_detail` extraction |
| Shells whose extracted core IS tested (`*Pure`/`*Helpers`/`*Mapping`/`*_detail`/`*Parse` siblings) | 4 | 2.5K | Small residual gap (HTTP orchestration only) |
| Test infrastructure (`Commands/Scenarios/*`, `*FixtureBackend`, fault injector) | 30 | 5.3K | Not a gap — this *is* the harness |

The codebase has a healthy, consistently-applied pattern: extract pure logic into a `*Pure.cpp` / `*_detail.cpp` sibling and doctest it, leaving a thin I/O shell (e.g. `PlaneActivityFeed.cpp` shell vs tested `PlaneActivityFeedPure.cpp`). The gap list below is, to first order, **the units where that extraction hasn't happened yet**.

## Tier 1 — high-risk uncovered logic (do these first)

> **Campaign status (2026-07-05, PRs #1604 / #1607 / #1609 / #1616):** rows #1–#6 and #8 are DONE — each extracted behind a tested pure seam (`TicketGridDurationSortPure`, `TicketFieldEditorLongTextPure` seed/commit, `TrackerGridFieldDisplayPure`, `TrackerDateTimePure` picker/commit, `JqlSuggestEnginePure` + `PlaneQuerySuggestEnginePure`, `MergeWatchNotifyPure`, `AiPrefsTestConnectionPure`). Rows #7/#9/#10 were assessed and closed without extraction: their decision logic already lives in tested units (`BugReportBody`, `CacheEvictionPolicy`/`IconDimensionsPolicy`/`ImageDimensionsPure`, `FieldEditPipelineService`/`OfflineQueueService`/`IssueDraftHelpers`); the residue is AppController/ImGui glue covered by the Tier-4 service-extraction track. **Campaign closed 2026-07-05 (PR #1617)**; plan doc archived to [`docs/plans/shipped/coverage-gap-tier1-pure-extractions.md`](docs/plans/shipped/coverage-gap-tier1-pure-extractions.md). The per-file ≥90% ratchet additions (sequencing step 6) remain deferred until CI publishes measured rates for the new units.


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

## Tier 2 — backend client HTTP shells (~3.4K LOC)

`GitHubClient.cpp` (673), `GitHubIssueSearch.cpp` (631), `PlaneIssueSearch.cpp` (708, audit-flagged), `PlaneIssueMutation.cpp` (570), `LinearClient.cpp` (556), `LinearIssueSearch.cpp` (374, security-flagged), `LinearIssueMutation.cpp` (239), `PlaneClient.cpp` (158), `Vcs/GitHubCommits.cpp` (96).

The mapping/JQL-translation halves of all three backends are well covered (`*MappingPure`, `*QueryFromJql`, `GitHubFetchPlan`, `GitHubIssueSearchMapping`, `GitHubClientHelpers`, `GitHubCommitsParse` all tested). What's untested is pagination, retry/error classification, and request orchestration — exactly what backlog item **B2** (`TrackerHttpClient` migration, batches 2B–2E) is about to churn. Land a fake-`TrackerHttpClient` fixture test per shell **before** each B2 batch; the existing `JiraFakeTrackerFixture.test.cpp` + `tests/fixtures/{plane,github,linear_backend}/` are the template, and `JiraIssueSearch.cpp`/`JiraIssueMutation.cpp` (both tested) prove the pattern scales to a full backend.

## Tier 3 — Commands strict zone (24 TUs, 4.6K LOC)

`Commands/` is a strict lint zone and the single registry feeding four frontends (CLI, palette, MCP, Lua), yet no `Builtin/BuiltinCommands_*.cpp` handler TU is compiled into a test target (registry plumbing — `CommandRegistry`, `Command`, `FuzzyMatch`, `PaneCommands_detail` — is tested; MCP dispatch is tested on the plugin side). *Update 2026-07-05:* PR #1618 added exactly the recommended table-driven harness — `Commands/Scenarios/CommandContractSweepScenario.cpp` registers every builtin against the fixture backend and asserts the error-envelope contract — but it runs in the **scenario lane**, so the 24 `BuiltinCommands_*` handler TUs are still not compiled into `SmatchetTests` (the TU metric is unchanged). It de-risks backlog item **N6** (splitting `BuiltinCommands.cpp` — done, see below) plus the `ui-request-flag-off-thread` race class the lint gate exists for; a per-TU direct-compilation harness would still push the 4.6K LOC into the OpenCppCoverage denominator.

## Tier 4 — AppController family (~7.2K LOC)

`AppController.cpp` (2835), `AppController_LuaBindings.cpp` (1528), `_LuaBindings_Draw.cpp` (1294), `_CatalogAndFieldEdit.cpp` (1124) + small satellites. Don't try to unit-test the monolith — the backlog already has the right plan: keep extracting friend-coupled services behind interfaces (**B1**, **N4**, **N8**; `TicketSyncService`/`OfflineQueueService`/`FieldEditPipelineService`/`AppController_LuaBindingsCore` are extracted *and tested*, proving the loop works). Each extraction PR should carry its golden tests per the B1 note ("testless extraction = lottery").

## Tier 5 — UI draw layer (62 TUs, 28.5K LOC)

Excluded from ctest coverage by explicit policy (`coverage.sh` excludes `Source*Core*src*Ui`; coverage-threshold-graduation Slice 1). The sanctioned mechanisms are (a) the `_detail.cpp` extraction pattern — 10 tested `_detail`/pure UI TUs already exist (`SmatchetGridHeaderUi_detail`, `SmatchetAutocompleteUi_detail`, `AnnotateAnalysisUi_Window_detail`, …) — and (b) the bucket-C/E scenario lanes, **blocking again as of 2026-07-05** (PR #1619 block-on-any-red; the Mesa-GL boot failure is resolved). Two consequences:

- The bucket-C/E lane is bootable and gating merges again, re-arming 30 scenario TUs + 5.3K LOC of existing harness and closing the Pillar-4 visual-validation gap that previously fell to humans. (Historically this section's highest-leverage action was *restoring* that lane — now done.) The residual UI-coverage work is continued `_detail` extraction to pull draw-layer logic into the ctest-instrumented denominator.
- Prefer `_detail` extraction whenever a UI TU is touched; `SmatchetFieldIconRender.cpp` (737 LOC, 2 audit findings — both now fixed, see `CPP_CODE_AUDIT.md` #14/#22) and `AnnotateAnalysisUi_Modals.cpp` (428, audit-flagged #23, fixed) are the two UI TUs with recently-fixed defects and no extracted core.

## Hygiene notes

- ~~`Source/Core/src/Test_JqlProjectScope.cpp` (49 LOC) is a pre-harness relic … Delete it.~~ **Done 2026-07-05** — the file no longer exists; `tests/Core/JqlProjectScope.test.cpp` is the surviving coverage.
- Backlog reconciliation: `BACKLOG_CODE_REVIEW.md` **N11** ("no tests/ directory exists", "single highest-leverage open item") is fully stale — 273 test files across 8 suites now exist; every N11 candidate unit (FuzzyMatch, MarkdownConvert, JqlProjectScope, TextMerge, CompactDateFormat) is covered. **N7** (MarkdownConvert golden tests) is likewise closed (`MarkdownConvert.test.cpp` + `fuzz_markdown_adf`). Both entries should be flipped on the next backlog pass.

## Recommended sequencing

1. ~~**Pair with the audit remediation** — findings #1 (long-text truncation) and #3 (duration-sort loop) land in Tier-1 TUs; fix + Pure-extraction + regression test in the same PR each.~~ **Done 2026-07-05** — both fixed (PR #1593) and pinned behind pure seams (`TicketFieldEditorLongTextPure`, `TicketGridDurationSortPure`, PR #1604).
2. **`TrackerGridFieldDisplay` + `TrackerDateTimeFieldEditor` extraction tests** (Tier 1 #3–#4) — the two big untested untrusted-input formatters/parsers (2 PRs).
3. **Backend-shell fixture tests ahead of each B2 batch** (Tier 2) — sequence with the existing backlog plan rather than as standalone work.
4. **One command-invocation harness PR** (Tier 3).
5. ~~**Bucket-C/E lane restoration** (Tier 5) — infra work, biggest single unlock.~~ **Done 2026-07-05** — lanes boot and gate merges again (PR #1619 block-on-any-red). Residual: continued `_detail` extraction to move UI draw logic into the ctest denominator.
6. Opportunistic: add each newly-tested trust-boundary unit to the `coverage-perfile-gate.sh` `HIGH_RISK_UNITS` ratchet so it can't rot back (the gate's own contract: "add a unit only with a test that brings it to ≥90% in the SAME change").

## Prior-state corrections recorded

None of this map's numbers contradict the prior audits; it complements them by measuring *testability reach* rather than defect presence. The one factual correction to circulating docs is the stale N11/N7 state noted above.

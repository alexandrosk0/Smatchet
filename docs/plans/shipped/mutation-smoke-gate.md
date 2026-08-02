# Plan — Mutation-smoke gate (testing-surface-roadmap Slice F)

> **Slug**: `mutation-smoke-gate` (matches this file's basename without `.md`).
>
> **Status**: `shipped` (2026-07-16) — **all 4 phases complete.** Phases 1 + 2 (harness + seed corpus; advisory nightly wiring + bats + local mirror, owner's decisions 2026-07-10: **advisory nightly → graduate to blocking after clean runs**, **80% floor**). Phase 3 (corpus expansion to the full dedicated-test TU set) shipped 2026-07-13 — 38 mutants (33 `killed` guards + 5 `equivalent`) covering all 20 dedicated-test TUs. **Phase 4 (blocking graduation) shipped 2026-07-16** — after 3 consecutive clean advisory nightly runs (2026-07-14 `1d78092`, 07-15 `2966edc7`, 07-16 `3704139`, each 33/33 killed @ 100% adjusted kill rate, 0 mis-ruled equivalents), `continue-on-error` was removed from the `tsan-linux-nightly.yml` mutation-smoke step so a sub-floor survivor now reds the nightly. See § Implementation log.
>
> **Roadmap**: [`testing-surface-roadmap.md`](testing-surface-roadmap.md) Slice **F** (Gap 4, "Mutation-smoke / coverage-delta gate"). **Precursor**: [`MUTATION_PILOT.md`](../../audits/MUTATION_PILOT.md) (2026-07-05). **Originating backlog**: `docs/self-improvement/categories/tooling/2026-07-05-mutation-harness-slice-f.md`.

## Context

`TEST_COVERAGE_GAP_MAP.md` measures **reachability** — which TUs a test compiles/links against (137/297). It cannot see whether the tests that reach a TU would **catch a bug** in it. The `Test-delta gate` has the same blind spot: it requires a *touched file* to carry a test delta, not that the delta actually *asserts* the changed behaviour. `CPP_CODE_AUDIT.md` finding #2 lives in an untested branch of a TU that is fully "covered" by the reachability metric.

Mutation testing measures the missing half — **assertion strength**. Flip one line of production logic (invert a compare, drop a guard, off-by-one a bound), rebuild, rerun the suite: **KILLED** = an assertion caught it (good); **SURVIVED** = a bug of that shape would ship uncaught (weak assertion, equivalent mutant, or out-of-oracle blind spot).

The 2026-07-05 pilot (`MUTATION_PILOT.md`) proved this is **cheap and deterministic** on this repo: 68 single-point mutants across 12 pure TUs against `SmatchetTsanTests` (`ninja-tsan-linux`, the only Linux-runnable doctest oracle), ~0.6 s/run + ~2–8 s incremental build, **zero residual tree dirt**, 82.5 % equivalent-adjusted kill rate. It found 10 genuine weak assertions (3 fixed, 7 backlogged) — assertion rot the coverage-delta gate structurally cannot see. This slice productionises that manual harness.

## Approach

A JSON corpus of `{id, file, search, replace, expect}` single-point mutants driven by a harness that, per mutant: asserts the tree is clean → confirms `search` occurs **exactly once** → applies the exact edit → incrementally rebuilds (1 TU + relink) → runs the rig → classifies KILLED / SURVIVED / BUILD_FAIL → reverts → re-asserts clean. An `EXIT` trap reverts on interruption; a clean-tree assertion brackets every mutant, so **no mutant is ever left in the tree**.

Three `expect` classes, so the gate is honest and un-gameable:
- **`killed`** — a regression guard. The current tests kill it; a *survival* means an assertion was weakened. Scored against the floor.
- **`survived`** — a **known** gap (backlogged, not yet fixed). Excluded from the floor (informational). When its backlog fix lands, the rig kills it and the harness nudges "flip expect → killed" so it graduates to a guard.
- **`equivalent`** — changes no observable behaviour; unkillable by any test. Excluded from the floor, but still applied-and-checked: a *killed* equivalent means the ruling is stale (fails honesty check).

The floor is a **kill-rate over the `killed` set** (assertion-strength), deliberately **not** per-line coverage — it catches the rot coverage can't.

## Files to modify

1. `scripts/dev/mutation-smoke.sh` (**new, shipped Phase 1**) — the harness. `--list` / `--dry-run` (no build; validates unique-search + apply/revert mechanics on any host) / full sweep / `--gate` (fail below floor) / `--floor N` / `--preset` / `--exe` / `--id`. Bypass `SMATCHET_SKIP_MUTATION_SMOKE=1`.
2. `scripts/dev/mutation-smoke-corpus.json` (**new, shipped Phase 1**) — seed corpus: 10 mutants transcribed from `MUTATION_PILOT.md` — 3 `killed` regression guards (GR3, JQL-05-email, PLANE-04, the pilot's fixed survivors), 3 `survived` known gaps (GR5, GR6, MergeWatch-m3), 4 `equivalent` (DT2, DT5, MAP-05, Labels-m3, the pilot's full equivalent set).
3. `.github/workflows/tsan-linux-nightly.yml` (**shipped Phase 2**) — an advisory (`continue-on-error`) `Mutation-smoke gate` step appended to the existing `tsan-linux` job, after ctest, reusing its built `ninja-tsan-linux` tree; runs `mutation-smoke.sh --gate --floor 80` on nightly + manual dispatch only.
4. `tests/bats/mutation_smoke.bats` (**shipped Phase 2**) — 9 unit tests of the classify/floor/honesty/tree-safety logic with a fake `cmake` PATH shim + fake exe (no real build), the way `merge_gates.bats` fakes `gh`.
5. `scripts/dev/test-mutation-smoke.sh` (**shipped Phase 2**) — local/CI mirror auto-enrolled via the `test-*.sh` glob (test-all.sh + Agentic self-tests): corpus JSON validity + `--list` + (clean-tree) `--dry-run` mechanics. No TSan build needed.

## Existing utilities reused

- `SmatchetTsanTests` / `ninja-tsan-linux` — the assertion oracle (unchanged).
- `MUTATION_PILOT.md` § "Every surviving mutant — exact diff" + § "Equivalent mutants" — the source of every corpus entry's `search`/`replace`/`expect` and the equivalent-mutant exclusion list (DT2/DT5/MAP-05/Labels-m3/JQL-01).
- The `SMATCHET_SKIP_*` bypass + `--gate`/advisory conventions from the merge-gate/coverage-gate scripts.

## Decisions (resolved 2026-07-10)

1. **Cadence** → **advisory nightly.** Wired as a step in the existing `tsan-linux-nightly.yml` job (reuses its configured+built `ninja-tsan-linux` tree), gated `github.event_name != 'pull_request'` so it runs on the nightly cron + manual dispatch only, not the workflow's paths-scoped PR trigger.
2. **Floor** → **80 %** over the `killed` guard set (`mutation-smoke.sh --gate --floor 80`).
3. **Graduation** → **advisory-first** (`continue-on-error: true`), graduate to blocking after N clean nightly runs (Phase 4). Matches dup-audit / comment-noise rollout.
4. **Corpus scope** → **10-mutant seed now**, expand to the full 17-TU set in Phase 3.
5. **TSan-runtime provisioning** → **moot on CI.** The `tsan-linux-nightly` job already builds `ninja-tsan-linux` green on `ubuntu-latest` (the runner's clang bundles compiler-rt/tsan); the runtime was only absent in the local dev container. No extra `apt-get` needed.

## UX Pillar callouts

N/A — CI tooling + test-harness only; no `Source/Core/` runtime surface, no UI, no perf-relevant path. Pillars 1–4 unaffected.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

`N/A` — the diff is a shell harness + JSON corpus + (Phase 2) a CI workflow + bats. No `Source/Core/` production code is touched. The corpus's `search`/`replace` strings *name* production lines but the harness only mutates them transiently in CI and always reverts (tree-clean-asserted); nothing is committed.

## Risks / non-goals

- **Non-goal:** replacing the coverage-delta / Test-delta gates. This is the complementary assertion-strength half; both stay.
- **Risk — corpus rot:** a production refactor changes a `search` line → `SPEC-ERROR (search not unique/absent)`. Mitigated: the harness fails loud (never silently skips-as-pass), and `--dry-run` in `test-all.sh` catches drift locally before CI.
- **Risk — equivalent misruling:** an `equivalent` mutant that gets killed fails the honesty check (`equiv_bad`), forcing a re-classification rather than silently inflating the rate.
- **Risk — floor gaming:** excluded by construction — the floor scores only `killed` guards; `survived`/`equivalent` are out of the denominator, so you cannot pad the rate with unkillable mutants.
- **Accepted:** Linux-oracle blind spot — mutants only killable by the Windows-only main rig (pilot LCM-06) are out of scope here; CI on Windows still catches them. Documented, not closed.

## Verification

Phase 1 (this PR), all run locally in-container:
- `shellcheck scripts/dev/mutation-smoke.sh` — clean.
- `python3 -c "import json; json.load(...)"` on the corpus — valid (10 mutants).
- `mutation-smoke.sh --list` — enumerates the corpus.
- `mutation-smoke.sh --dry-run` — all 10 mutants: unique-search confirmed, apply+revert mechanics OK, **tree verified clean afterward**. (The full KILLED/SURVIVED classification is CI-only — `SmatchetTsanTests` needs the TSan runtime absent from this container; the `expect` values are transcribed from the pilot and the first advisory nightly run validates them.)

## Out of scope (flagged, not designed)

- Per-line coverage-delta upgrade to the `Test-delta gate` (the roadmap lists it as the *other* half of Slice F's "either/or" — this plan takes the mutation-smoke half, which the pilot showed catches rot coverage cannot).
- Windows-rig mutation (main doctest rig / bucket-E) — needs an MSVC runner harness; separate slice if ever wanted.

## Implementation log

- `<phase-1-sha>` · Phase 1 — `mutation-smoke.sh` harness + `mutation-smoke-corpus.json` seed (10 mutants), dry-run-validated. CI wiring held for § Decisions.
- 2026-07-13 · Phase 3 — corpus expanded 10 → 38 mutants, now covering **all 20 dedicated-test TUs** in the TSan rig (the pilot-era "17-TU set" grew as new service TUs gained dedicated tests). Every entry **live-validated in-container** (`libclang-rt-18-dev` installed per the pilot's infra note; full build + 226-case suite green first). Changes:
  - **Stale `survived` expects flipped → `killed`** (GR5, GR6, MergeWatch-m3): their backlogged pinning assertions landed post-pilot, the harness's own "flip expect → killed" nudge confirmed all three now KILLED — they graduate to regression guards, exactly the § Approach lifecycle.
  - **4 more pilot-fixed survivors added as guards** (PLANE-03, JQL-03, LinearQueryFromJql JQL-05, LinearClientHelpers m5) + the pilot's 5th equivalent (JQL-01 `not in` offset) — the corpus now carries the pilot's complete vetted mutant set.
  - **23 new mutants over the 13 previously-uncovered TUs** (JiraErrorMessagePure, AiPrefsTestConnectionPure, TicketSyncService, EditMetaCacheService, FieldEditPipelineService, ConnectivityMonitorService, LocalCacheManager, GridLiveContext, OfflineQueueService, ConfigSaveWorker, AiAssistantController + scored guards for the equivalent-only TrackerDateTimePure/TrackerLabelsPure/LinearIssueMappingPure).
  - **1 new genuine weak assertion found + fixed** (the Phase-3 sweep's only real survivor): `JiraErrorMessagePure.test.cpp` "cap never splits a multi-byte UTF-8 sequence" used a 200×'é' = exactly-400-byte message that fit `<= kMaxJoinedErrorLen` and appended whole — the truncation backoff it documents never executed, so mutant JIRAERR-02 survived. Test now uses 300×'é' (600 B) and asserts the ellipsis marker (proof truncation engaged); SURVIVED → KILLED verified.
  - **1 mutant rejected as flaky** (honesty rule — no silent caps): a `ConfigSaveWorker` drain-loop dirty-flag mutant (CSW-02) classified KILLED or SURVIVED depending on worker-thread timing (3 SURVIVED / 2 KILLED over 5 runs) — a nondeterministic oracle has no place in a nightly gate; dropped. `CSW-01` (post-Stop synchronous-save path, synchronous assertion, 5/5 deterministic) keeps the TU scored.
  - **1 candidate skipped as out-of-oracle**: `GridLiveContext.h` `everVisible` default — consumed only by AppController (Windows-rig territory), so a survivor here would be a false gap (pilot LCM-06 class), and a permanently-unfixable `survived` entry can never graduate. Not added.
  - Final gated sweep: **killed=33 survived=0 build_fail=0, equivalents 5 checked / 0 mis-ruled, adjusted kill rate 100% (floor 80%)**, tree clean throughout.
- 2026-07-16 · Phase 4 — **blocking graduation.** After the Phase-3 corpus merged (#1818), three consecutive advisory nightly runs came back clean, verified at the step-log level (`continue-on-error` neutralises the step conclusion, so the `mutation-smoke: OK.` line is the authority, not the green check): 07-14 run `29313605417` (`1d78092`), 07-15 run `29396606994` (`2966edc7`), 07-16 run `29479599609` (`3704139`) — each **killed=33 survived=0, 5/5 equivalents surviving, 0 mis-ruled, 100% adjusted kill rate**. Removed `continue-on-error: true` from the `tsan-linux-nightly.yml` mutation-smoke step and dropped "advisory" from its name/comment, so a sub-floor survivor now reds the nightly. This closes Slice F's mutation-smoke half; the coverage-delta half stays out of scope (§ Out of scope).

## Deviations from plan

- The bats suite `tests/bats/mutation_smoke.bats` shipped without a `test-*.sh`
  wrapper naming its path, so the orphan-bats gate (`test-orphan-bats.sh`) failed
  on develop and the suite never ran in agentic-selftests. Fixed by adding
  `scripts/dev/test-mutation-smoke-bats.sh` (canonical bats wrapper); the corpus/
  harness mirror `test-mutation-smoke.sh` validates the script itself and does not
  count as a suite wrapper.

## Verification (actual)

- Phase 1: shellcheck clean; corpus valid JSON; `--dry-run` green with tree clean afterward (see § Verification).
- Bats wrapper: `test-mutation-smoke-bats.sh` runs `mutation_smoke.bats` 9/9 green; `test-orphan-bats.sh` PASS (all 63 suites wrapped); shellcheck clean.
- Phase 3 (2026-07-13, all in-container): `SmatchetTsanTests` baseline green (226 cases / 2150 assertions incl. the strengthened UTF-8-cap test); full gated sweep `--gate --floor 80` → 33/33 killed, 5/5 equivalents correctly surviving, 0 mis-ruled, exit 0, tree clean; concurrency-adjacent guards (AIC-01/AIC-02/CSW-01) re-run 3× each — deterministic; `test-mutation-smoke.sh` 4/4; `mutation_smoke.bats` 9/9; shellcheck clean.

## Archive (post-ship — DO IN THIS PR, never a follow-up)

- **Done in this PR (2026-07-16, Phase 4):** flipped `docs/self-improvement/categories/tooling/2026-07-05-mutation-harness-slice-f.md` → `applied` (archived into `applied.md`) and moved this plan to `docs/plans/shipped/`.

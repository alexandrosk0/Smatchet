# Plan — Mutation-testing pilot (assertion-strength probe)
<!-- plan-date: 2026-07-05 -->

> **Slug**: `mutation-testing-pilot`.
> **Status**: `shipped` — report + strengthening PR merged (#1626, squash `44ea33d`); this file lives in `docs/plans/shipped/`.
> **Owner**: orchestrator. **Created / executed**: 2026-07-05.

## Context

`TEST_COVERAGE_GAP_MAP.md` measures test **reach** (which TUs are compiled into a test target); nothing measures whether a reaching test would **catch a bug** — its own caveat cites `ConfigManager.cpp` finding #2 sitting in an untested branch of a compiled-in TU. This pilot probes assertion strength via mutation testing over the only assertion-based test executable that builds+runs on this Linux container. Outcome: a ranked weak-assertion map (`MUTATION_PILOT.md`), a PR fixing the 3 worst, and backlog for the rest. Precursor to roadmap Slice **F** (`testing-surface-roadmap.md`).

## Approach

Establish the runnable oracle (Phase 0), plant ~5 single-point mutants each across 10–15 in-scope TUs one at a time (build → run → record → revert, tree-clean-asserted), then report + strengthen the 3 worst + backlog the rest (Phase 2). No production code changes — a survivor caused by wrong production code is an audit finding to document, not fix. Trade-off: the TSan rig is a *subset* oracle (Windows-only tests excluded), so every survivor is cross-checked against the main-rig test before being called a gap.

## Files to modify

1. `MUTATION_PILOT.md` (new) — the report (per-TU kill rates, every survivor + diff + why, ranked list).
2. `tests/Core/JqlSuggestEnginePure.test.cpp` — email-prefix user-match assertion (kills JQL-05).
3. `tests/Core/PlaneQuerySuggestEnginePure.test.cpp` — account-id-insert assertion (kills PLANE-04).
4. `tests/Core/TrackerGridFieldDisplayPure.test.cpp` — full-page no-`*` assertion (kills GR3).
5. `docs/self-improvement/categories/{test,infra,tooling}/2026-07-05-*.md` — residual weak assertions, TSan-runtime gap, reusable harness.

## Existing utilities reused

- `SmatchetTsanTests` target + `ninja-tsan-linux` preset (`tests/CMakeLists.txt:60-291`) — the headless doctest oracle.
- Per-test `HasInsert` / `HasLabel` / `Run` helpers already in each suggest-engine test — new assertions reuse them, no new scaffold.

## Extraction sizing

N/A — this plan adds tests + docs, extracts nothing.

## Perf-gate section

N/A — no `Source/Core/` **production** change (test-only + docs). The three edits touch `tests/` only; the mutated production TUs are reverted, never modified.

## Verification

- Phase 0: `cmake --preset ninja-tsan-linux && cmake --build --preset ninja-tsan-linux` → `SmatchetTsanTests` green (209 cases / 2064 assertions / 0.59 s). Requires `libclang-rt-18-dev` (infra note filed).
- Phase 1: 68 mutants, tree `git status` clean between each and after every batch.
- Phase 2: new tests green clean (2068 assertions); each of the 3 target mutants re-run and confirmed SURVIVED→KILLED.

## Implementation log

- 2026-07-05 — Phase 0: 52 TUs reachable via `SmatchetTsanTests` (17 with dedicated in-rig tests); fuzz = crash-only, posix-core = compile-only. Scope fixed at 52 TUs.
- 2026-07-05 — Phase 1: 12 TUs, 68 mutants — 52 killed / 16 survived (76.5 % raw, 82.5 % equivalent-adjusted).
- 2026-07-05 — Phase 2: `MUTATION_PILOT.md` written; 3 worst survivors fixed (each proven SURVIVED→KILLED); 7 residual + infra + tooling backlogged.
- 2026-07-05 — Shipped: PR #1626 squash-merged to `develop` (`44ea33d`). The one unrelated flaky Mesa-GL `Mobile texture-guard smoke` lane was downgraded via the `tests-out-of-band` label; all in-scope lanes green. Plan archived active → shipped in this follow-up.

## Deviations

- Plan-lock seeded in Phase 2 rather than pre-Phase-0 (the pilot is analysis-first; no production risk accrued before the lock). Noted per process-rules.

## Self-improvement

See `docs/self-improvement/categories/{test,infra,tooling}/2026-07-05-*`.

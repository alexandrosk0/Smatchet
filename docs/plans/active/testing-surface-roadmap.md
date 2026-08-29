# Testing-surface roadmap — master sequencing plan

**Status:** active — sequencing meta-plan (review gate before per-slice execution).
**Owner:** orchestrator. **Created:** 2026-06-14. **Drives:** execution of the
[`testing-surface.md`](../../guides/testing-surface.md) §6 roadmap (8 items, P0–P3).

**Progress (2026-08-29):** shipped — **H, A, D, E1, C** (both phases), **E2**, **G**
(Phases 1–2), **F** (mutation-smoke gates the tsan nightly at `--floor 80`), **I**
(Phase 0, #1507). **B unblocked** — its exe-boot blocker was falsified 2026-06-18
and the all-gates-blocking flip already makes both Mesa lanes block on broken
harness; two narrow residuals remain (see § Deviations). Remaining — **B**
residuals, **G** Phase 3 / G2, **J**. Detail in § Implementation log + § Deviations.

## Purpose

`testing-surface.md` §6 lists 8 improvement items but does not order them into
shippable PRs, tag effort/risk, or say which need a detailed plan before code.
This doc does that — it is the **review-and-reorder gate**. The user approves (or
reshuffles) the slice order here; then each **non-trivial** slice gets its own
detailed plan doc *at execution time* (not all upfront — that wastes review), and
each **just-do** slice ships directly once its turn comes.

How to read: the **Slice catalog** is the contract; the **Recommended sequence**
is a proposal; **Per-slice scope** is one paragraph of what each PR actually does.

## Recon corrections to `testing-surface.md`

Grounding recon (2026-06-14) contradicts three load-bearing assumptions in the
guide. These change scope and are folded into the slices below; **each ships a
one-line correction back to `testing-surface.md` in the slice that resolves it.**

1. **Bucket-C/E are opaque monolithic checks.** Both run a single
   `Smatchet.exe scenario.run --name=ui-test` invocation
   (`.github/workflows/build-and-test.yml:576`) — **no per-test granularity, no
   ctest label**. The roadmap's "split stable scenarios from Mesa-flaky ones,
   quarantine the latter by name" (§6 P0) is **not directly doable**; it needs a
   scenario-enumeration refactor first (Slice B). The *cheap* P0 win is the
   already-backlogged launch-smoke (Slice A), which kills "dead-harness-as-flaky"
   without granularity. `QuarantineTestCase.h` exists in `tests/support/` — a
   quarantine seam at the C++ test layer, the hook Slice B builds on.
2. **HTTP fault-injection is cheaper than §5 Gap 2 says.**
   `JiraCatalogHttpFixture.h` is an **in-process httplib loopback server** (not a
   client stub) — the production `JiraClient` drives *real cpr* at it, so
   `TrackerHttpRequestWithRetry` retry + `startAt` pagination **are now wired into
   the live tracker paths and exercised by the fault-injection tests** (#1231;
   previously the retry path was effectively unreached), and the fixture can force
   status codes (429/500). The guide's "Fake
   fixtures stub the client interface, not the transport" is wrong for *this*
   fixture. So extending it (debt.md:65, [planned]) reaches most transport faults
   with **no new `IHttpTransport` seam**. Only timeout / SSL / truncated-body still
   want a real seam (deferred sub-slice D2). `TrackerHttpClient` itself is a
   retry-*wrapper* (takes a `requestFn` callback; cpr lives at the call site), so
   there is no existing pure-virtual to fake — confirming the seam-creation cost the
   guide implied is real but **avoidable for the common faults**.
3. **Two "quick wins" shrank.** (a) **Coverage is already blocking** —
   `coverage.yml:49` is `continue-on-error: false`, threshold 65 (graduated
   2026-06-04); it is simply **not in `required_contexts`**, so a direct REST merge
   bypasses it. "Add Coverage to required" = a branch-protection + Pattern-A change,
   not new gate logic. (b) **3 of the 6 worktree-skip scripts are already deleted**
   (`test-ui-callstack-tooltip`, `test-ui-ai-assistant`, `test-lint-hook-split` —
   gone from `scripts/dev/`); the `WORKTREE_INCOMPATIBLE_RE` regex carries **dead
   alternatives**. The 3 survivors are bucket-E drivers whose skip guards real
   worktree baseline-drift, **orthogonal to #1166's `GIT_EXEC_PATH` fix** → still
   justified. So Gap 7 = trim 3 dead alternatives + document "3 remain" (trivial),
   not "remove the skip".

## Slice catalog

Effort: **XS** ≤1 h · **S** ≤½ day · **M** ~1 day · **L** multi-day.
Type: **just-do** (ship directly) · **plan** (detailed plan doc → review → code).

| # | Slice | Roadmap item | Effort | Risk | Type | Depends | Backlog |
|---|---|---|---|---|---|---|---|
| **H** | Worktree-skip-6 re-audit: trim 3 dead regex alts, document 3 survivors | §6 P2 (Gap 7) | XS | none | **just-do** | — | applied.md:342, test.md:63 [planned] |
| **A** | Bucket-lane launch-smoke + zero-pass hard-fail | §6 P0 (Gap 1) | S | low | **plan** (short) | — | infra.md:40 `bucket-lane-launch-smoke` P1 [planned] |
| **D** | HTTP fault-injection: extend loopback fixture → retry/pagination integration tests | §6 P1 (Gap 2) | M | low | **plan** (short) | — | debt.md:65 [planned] |
| **E1** | Crafted-PNG-dims cap test in `GoldenImage.test.cpp` | §6 P1 (Gap 3, narrow) | XS | none | **just-do** | — | security.md:59-60 [planned] |
| **C** | Coverage + Sanitizer → `required_contexts` via Pattern A | §6 P0 | M | med | **plan** | — | [new] — no entry |
| **B** | Make bucket-E/C merge-gating: enumerate scenarios + quarantine lane | §6 P0 (Gap 1) | L | med-high | **plan** | A | infra.md:40 (superset) |
| **E2** | libFuzzer harness: new preset + 6 parser targets + seed corpus + CI lane | §6 P1 (Gap 3) | L | low | **plan** | E1 | security.md:59-60 (superset) |
| **G** | DB-corruption corpus test (Gap 6) + LocalCacheManager integrity hardening | §6 P2 (Gap 6) | M | low-med | **plan** | — | [new] — unbacklogged |
| **J** | Perf-fast widen: `rows[]` retrofit on 8 scenarios + touched-scenario rule | §6 P3 (Gap 8) | M | low-med | **plan** | — | applied.md:265 [planned, narrower] |
| **F** | Mutation-smoke / coverage-delta gate (false-GREEN half of Gap 4) | §6 P2 (Gap 4) | M-L | med | **plan** | — | [new] — unbacklogged |
| **I** | Agent-eval Phase 0 (judge calibration) | §6 P3 (Gap 5) | L | low | **plan exists** | — | tooling.md:505-507 + [`subagent-eval-agentic-coverage.md`](subagent-eval-agentic-coverage.md) |

## Recommended sequence + rationale

Ordered by **leverage ÷ cost**, honouring dependencies and front-loading additive
(zero-merge-risk) work before team-wide gating changes.

1. **H** (just-do, XS) — clears a stale doc gap + dead regex today; zero risk.
2. **A** (plan, S) — highest-leverage P0 that's actually cheap; restores *trust* in
   the advisory lanes (catches dead-harness) and is the prerequisite mindset for B.
3. **D** (plan, S/M) — pure-additive HTTP integration tests; **also recovers backend
   coverage**, shrinking the `coverage-out-of-band` label class → de-risks Slice C.
4. **E1** (just-do, XS) — additive parser cap test; closes the cheap half of Gap 3
   and seeds the corpus mindset for E2.
5. **C** (plan, M) — now that D has lifted coverage, promote Coverage + Sanitizer to
   required via Pattern A. Team-wide gating change → full plan + careful rollout.
6. **B** (plan, L) — the architectural P0: enumerate `ui-test` into discrete named
   scenarios, wire `QuarantineTestCase` quarantine lane, drop blanket
   `continue-on-error` on the stable subset. Depends on A's launch-smoke trust.
7. **E2** (plan, L) — libFuzzer harness once E1 proves the corpus shape.
8. **G** (plan, M) — DB-corruption: test exposes the gap, then harden
   `LocalCacheManager` open path (integrity_check + rebuild fallback).
9. **J** (plan, M) — perf-fast widening; needs the 8-scenario `rows[]` retrofit first.
10. **F** (plan, M-L) — mutation-smoke gate; valuable but lowest leverage-per-cost.
11. **I** (existing plan) — agent-eval Phase 0; heavy + already separately planned;
    execute when the user prioritises the fleet over product-test coverage.

**Natural review checkpoints:** after H+A+D+E1 (the additive/trust block) is a good
pause to re-confirm before the gating-policy slices (C, B) that affect all sessions.

## Per-slice scope (one paragraph each)

- **H — Worktree-skip re-audit.** Edit `WORKTREE_INCOMPATIBLE_RE` in
  `scripts/dev/test-all.sh`: drop the 3 deleted-script alternatives; leave the 3
  live bucket-E drivers with a refreshed comment ("worktree baseline-drift; #1166
  build fix is orthogonal"). Update `testing-surface.md` Gap 7 + §5.1 to "resolved:
  regex trimmed, 3 survivors justified". Mark the applied.md/test.md re-audit row
  done. **No product code.** Ships as a tiny PR.
- **A — Launch-smoke.** Per infra.md:40 concrete action: (1) add a
  *non*-`continue-on-error` step after Mesa install + before each advisory bucket in
  `build-and-test.yml` that runs the freshly-built exe once (`app.version --spawn
  --yes`, ≤10 s) so "exe can't start" hard-fails; (2) make
  `test-screenshot-diff.sh` + the bucket-E driver hard-exit on `Passed==0 &&
  Failed>0`. Short plan: the exact step placement + the driver guard. Updates the
  postmortem owed for the 2026-06-07 dead-harness incident.
- **D — HTTP fault-injection.** Extend `JiraCatalogHttpFixture.h` with forced
  429/500/partial-page routes + search/mutation/user-meta endpoints; add
  `tests/Core/*` integration tests asserting `TrackerHttpRequestWithRetry`
  backoff/retry count + `startAt` pagination assembly against the loopback server.
  Short plan: route list + assertion matrix. **Shipped as Slice D+D3 (#1231):** the
  `TrackerHttpRequestWithRetry` retry path is now wired into the live tracker paths
  and exercised by these fault-injection tests (it is no longer the unreached
  "already exercised" code the recon assumed). Defers timeout/SSL/truncated-body to a
  **D2** (needs a real transport seam) — flagged, not built here.
- **E1 — PNG-dims cap test.** New `tests/Core/GoldenImage.test.cpp` (or extend
  existing): craft PNG byte headers at / above `kMaxGoldenImageDim = 16384`, assert
  `ParseImageDimensionsFromBytes` rejects. ~30 min per security.md:59-60. Just-do.
- **C — Coverage + Sanitizer required.** Refactor `coverage.yml` + the ASan/UBSan
  jobs to Pattern A (no workflow `paths:` filter; internal change-detection → green
  no-op when irrelevant) so they *always report* and can join `required_contexts`
  without path-filter deadlock; add the 2 check names to
  `project.config.json` branch_protection + the live branch-protection ruleset. Full
  plan: rollout order, flake-budget, the `*-out-of-band` escape hatches.
- **B — Bucket-E/C gating.** Split `scenario.run --name=ui-test` into per-scenario
  invocations (or a ctest-label harness) so individual UI scenarios pass/fail
  discretely; route Mesa-flaky ones through `QuarantineTestCase`; drop blanket
  `continue-on-error` on the stable subset; thread a launch-smoke (from A) as the
  hard gate. Full plan: enumeration mechanism, quarantine registry, required-check
  naming. Architectural — the heaviest P0.
- **E2 — libFuzzer harness.** New CMake `-fsanitize=fuzzer` target(s) reusing the
  `ninja-clang-asan` toolchain; fuzz drivers for `AiSseParser::Feed`,
  `AiNdjsonParser::Feed`, `CppLexLine`, `ParseCallstackText`, `MarkdownToAdf`,
  `ParseImageDimensionsFromBytes`; seed corpora in `tests/fixtures/`; short run in
  PR (ASan lane) + long run nightly. Full plan: preset, corpus layout, CI lane,
  crash-triage path.
- **G — DB-corruption.** Characterization test first (point `LocalCacheManager` at a
  truncated/garbage DB via its constructor path param — the `SqliteMemFixture` seam
  proves arbitrary paths work — and capture current behaviour, likely throw/crash).
  Then harden the open path: `PRAGMA integrity_check` + rebuild-from-empty fallback
  + `SQLITE_BUSY` storm handling beyond the existing 5 s `busy_timeout`. Product
  change in `Source/Core/src/Persistence/` → **perf-gate section required** in its
  plan. Routes to `offline-sync` specialist.
- **J — Perf-fast widen.** Retrofit `rows[]` emission on the 8 of 15 scenarios that
  don't emit them (applied.md:265 — prereq), then either widen the PR-fast subset or
  add a "touched-scenario-not-in-fast-set ⇒ run it" rule to the `perf-gatekeeper`
  diff→scenario map. Plan: which 8, the `rows[]` retrofit pattern, the gatekeeper
  rule. **Perf-gate section required.**
- **F — Mutation-smoke gate.** Either a flip-and-rerun mutation smoke on the hottest
  pure files (expose no-op assertions) or upgrade the `Test-delta gate` to require a
  *coverage delta on changed lines*, not just a touched file. New tooling. Full plan.
- **I — Agent-eval Phase 0.** Execute Phase 0 of the existing
  [`subagent-eval-agentic-coverage.md`](subagent-eval-agentic-coverage.md) (judge
  calibration loop) — do **not** re-plan; that doc is authoritative.

## Which slices need a detailed plan before code

**just-do** (no separate plan; ship directly when their turn comes): **H, E1**.
**plan-doc required** (detailed plan → user review → code): **A, D, C, B, E2, G, J,
F**. **I** uses its existing plan.

For each plan-doc slice I will, at execution time: author
`docs/plans/active/<slice-slug>.md`, present it for review, and only code on
approval. Slices touching `Source/Core/` (G, J, and E2's drivers) carry the
mandatory **Perf-gate** section.

## Doc-correction follow-ups (to `testing-surface.md`)

Each lands in the slice that resolves it (don't batch into a separate PR):
- Slice A/B → fix §5 Gap 1 + §2 bucket-C/E rows to note the opaque-monolith reality
  and the launch-smoke/enumeration distinction.
- Slice D → fix §5 Gap 2 + §5.1 row 2: the loopback fixture *does* reach transport;
  reframe the void as timeout/SSL/truncated-body only.
- Slice C → fix §3 Coverage row: "blocks its own job" → "blocks + now required".
- Slice H → fix §5 Gap 7 + §5.1 row 7 to "resolved".

## Implementation log

- 2026-06-14 — Master plan drafted from 3-agent recon. Awaiting user review of slice
  order before any slice executes.
- 2026-06-14 — User approved sequence `H→A→D→E1→C→B→E2→G→J→F→I` ("Approve + start
  additive block"): ship H now, author Slice A plan for review, then stop for
  re-confirmation after the additive block (H+A+D+E1) before gating slices C/B.
- 2026-06-14 — **Slice H done** (#1214). `WORKTREE_INCOMPATIBLE_RE` in
  `scripts/dev/test-all.sh` trimmed 6→3 alternatives (3 dead scripts removed:
  `test-lint-hook-split`, `test-ui-callstack-tooltip`, `test-ui-ai-assistant`); 3
  survivors kept + justified (bucket-E UI drivers, skip orthogonal to #1166).
  `testing-surface.md` §5 Gap 7 + §5.1 row 7 + §6 follow-up marked resolved.
  Verified: `test-docs.sh` 13/13, `shellcheck test-all.sh` clean.
- 2026-06-14 — **Slice A done** (#1180 / #1183 / #1187). Bucket-lane launch-smoke +
  zero-pass hard-fail landed across three independent PRs (not the single short-plan
  PR the slice catalog assumed): the launch-smoke step in `build-and-test.yml` plus
  the `Passed==0 && Failed>0` driver guards. Restores trust in the advisory lanes
  (catches dead-harness).
- 2026-06-14 — **Slice D done** (Slice D+D3, #1231 / 377c9f72). HTTP fault-injection:
  extended `JiraCatalogHttpFixture.h` with forced 429/500/partial-page routes +
  `tests/Core/*` integration tests; **wired `TrackerHttpRequestWithRetry` into the
  live tracker paths** so the retry/backoff + `startAt` pagination are now genuinely
  exercised. Shipped combined as D+D3 rather than D-then-deferred-D3.
- 2026-06-14 — **Slice E1 done** (#1240 / 5f680668). Crafted-PNG-dims cap test +
  `kMaxGoldenImageDim = 16384` enforcement: `ParseImageDimensionsFromBytes` rejects
  over-cap headers, exercised by crafted-header tests.
- 2026-06-14 — **Slice C done** (Phase 1, #1253 / af475041). Coverage + both
  sanitizers (ASan/UBSan) promoted to required contexts via Pattern A — they always
  report and now join `required_contexts` so a direct REST merge can no longer bypass
  them.
- 2026-06-16 — **Slice C complete** (Phase 2). After #1253 merged (2026-06-15) and
  `develop` went green on all three contexts, the live `develop` branch-protection
  ruleset was flipped 6 → 9 via `setup-branch-protection.sh` (repo-admin action, no
  PR), making `Coverage` + `Sanitizer (ASAN via MSVC)` + `Sanitizer (UBSan via Clang)`
  required. Slice C fully shipped; plan archived to
  `docs/plans/shipped/coverage-sanitizer-required-contexts.md`.
- 2026-06-16 — **Slice E2 done** (#1296 → #1301 → #1307; ran *ahead of* B — see
  Deviations). libFuzzer harness over the 6 untrusted-byte parsers. E2a (#1296) landed
  the `ninja-fuzzer-linux` preset + `tests/fuzz/` scaffold + `smatchet_add_fuzz_target`
  helper + advisory `fuzz-smoke.yml` + `fuzz_image_dims`; E2b shipped the remaining 5
  drivers as PR 2a (#1301 — `fuzz_cpp_lex` / `fuzz_callstack` / `fuzz_markdown_adf`) +
  PR 2b (#1307 — `fuzz_ai_sse` / `fuzz_ai_ndjson`). Surface complete; plans archived to
  `docs/plans/shipped/slice-e2-libfuzzer.md` + `slice-e2b-libfuzzer.md`.
- 2026-06-17 — **Slice G Phases 1–2 done** (#1327, #1352; ran *ahead of* B — see
  Deviations). Phase 1 (#1327) characterization-pinned the `LocalCacheManager`
  corrupt-file-on-open crash (uncaught ctor `SQLite::Exception` — Pillar 3); Phase 2
  (#1352) made the ctor survive it (pre-open `PRAGMA schema_version` probe → quarantine
  `.corrupt-*` + `-wal`/`-shm` sidecars → rebuild fresh), flipping the Phase-1 cases to
  expect graceful rebuild. **Phase 3 / G2** (`SQLITE_BUSY` contention + config open
  path) not yet greenlit — see the plan at
  (`docs/plans/shipped/slice-g-db-corruption.md`).
- 2026-07-14 — **Slice F substantially shipped** (status reconcile; work landed under
  `docs/plans/mutation-smoke-gate.md`, not this roadmap's row). The
  mutation-smoke half of Gap 4 is built: harness + seed corpus (Phases 1–2), advisory
  nightly wiring, and the 2026-07-13 Phase-3 corpus expansion to 38 mutants covering
  all 20 dedicated-test TUs (33/33 killed). Remaining under F: the advisory→blocking
  graduation (that plan's Phase 4) — the coverage-delta-gate alternative named in the
  catalog was not pursued.
- 2026-07-14 — **Slice I done** (status reconcile). Agent-eval Phase 0 (judge
  calibration) shipped via #1507 — see
  `docs/plans/subagent-eval-agentic-coverage.md` § Status;
  that plan's Phases 1–4 continue as its own scope, outside this roadmap's Slice-I row.

## Deviations

- **Slice A — shipped via three independent PRs (#1180 / #1183 / #1187)** instead of
  the single "short plan → one PR" plan-doc-per-slice flow the catalog prescribed.
  The launch-smoke + zero-pass hard-fail work split naturally across the CI-step,
  driver-guard, and screenshot-diff seams; each landed on its own.
- **Slice D — shipped combined as Slice D+D3 (#1231).** The catalog scoped D as the
  loopback-fixture extension with D2/D3 (real transport seam) deferred; in practice
  wiring `TrackerHttpRequestWithRetry` into the live paths shipped together with the
  fault-injection tests in one PR.
- **Sequence reordered — E2 + G shipped ahead of B.** The approved order
  (`…C→B→E2→G…`) put B before E2 and G, but B was believed **blocked** at the time
  (next bullet — the blocker was later falsified), so the
  additive, zero-merge-risk slices E2 (#1296/#1301/#1307) and G (#1327/#1352) were
  pulled forward rather than stall the roadmap behind B's prerequisite. Leverage-per-
  cost ordering otherwise preserved; only B's position changed.
- **Slice B — unblocked and mostly superseded (status corrected 2026-08-29).**
  The originally-stated blocker was falsified on 2026-06-18: the Mesa software-GL
  exe **boots in ~2 s** under llvmpipe (the ~26 s "can't boot" was a since-fixed
  `--spawn` exit-code bug — see `bucket-mesa-exe-boot` in
  `docs/self-improvement/categories/applied.md`). The all-gates-blocking flip
  (`MERGE_GATES_BLOCK_ALLOWLIST_RE="."`,
  `agents/scripts/core/merge-gates.d/00-common.sh:42`) then retired the
  meant-to-block allow-list entirely — every check blocks unless its NAME contains
  `advisory`, and neither bucket lane carries a job-level `continue-on-error` any
  more. The lanes' broken-harness teeth (zero-pass, lane-integrity) already BLOCK;
  only two documented step-level masks keep the render verdicts advisory. Residual
  B work is therefore narrow: **(a)** fix or skip the ~3/74 render-dependent
  bucket-E tests under llvmpipe so the per-test step mask
  (`.github/workflows/build-and-test.yml` bucket-E per-test run) can go; **(b)**
  establish CI-native goldens (provenance report: 0/7 CI-native today) so the
  bucket-C per-scenario golden-diff mask can go. Slice A's launch-smoke —
  graduated to a dedicated BLOCKING check (#1375) — covers dead-harness meanwhile.

## Verification

This meta-plan is docs-only — no build/test. Per-slice verification lives in each
slice's own plan doc. Sequencing acceptance = user sign-off on the order above.

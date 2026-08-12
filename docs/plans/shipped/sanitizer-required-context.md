# Plan — `sanitizer-required-context`: promote both Sanitizer PR lanes to required
<!-- plan-date: 2026-06-15 -->

Slug: `sanitizer-required-context` · Owner: orchestrator · Started: 2026-06-15 · Loop-mode: `in`

> **Status**: shipped — Slice 1 superseded by #1277 (duplicate-effort collision #2),
> Slice 1b shipped via #1280 (merged), Slice 2 superseded by #1253, side-task
> (bucket down-scope) shipped via #1273 (merged). All goals achieved on `develop`;
> see § Implementation log + § Verification for per-slice disposition.
>
> **RE-SCOPED 2026-06-15 — Slice 2 SUPERSEDED.** Mid-flight recon found a parallel
> `Slice C` effort ([`coverage-sanitizer-required-contexts.md`](docs/plans/coverage-sanitizer-required-contexts.md))
> already promoted BOTH Sanitizer lanes to **live** branch-protection required
> contexts: PR #1253 (`af475041`, merged 2026-06-15 12:46) added them to config
> `required_contexts` + `ci.required_checks` + escape hatches + the deterministic-red
> ctest fix; Phase 2 already ran (live `develop` ruleset shows 9 contexts, both
> sanitizers present). Slice C also chose **Pattern C** (job-level `if:` skip →
> GitHub treats skipped-required as success), NOT the Pattern-A self-gate this plan
> had drafted — so Slice 2 is not only done but my approach was wrong. **This plan is
> re-scoped to the half Slice C did NOT cover: ASAN-lane fixture/assertion *fragility
> hardening*** so the now-live required ASAN lane can't false-red on a slow runner —
> Slice 1 (ADF fixture, PR #1273) + Slice 1b (9 wall-clock timing-assertion guards).

## Problem

The `Sanitizer (ASAN via MSVC)` + `Sanitizer (UBSan via Clang)` per-PR lanes are
on the merge-gates **poller** block-list (meant-to-block) but are NOT GitHub
required contexts. Every GitHub-native merge path that gates on required
contexts only — `gh pr merge --auto`, the merge button, admin/direct
`gh api …/merge` — sails past a red Sanitizer. PR #1220 auto-merged past a
terminal-RED ASAN lane (no override label); #1229 escaped the same lane the same
day. This is the exact structural hole #923/#1130 fixed for **Coverage**
(option A, self-gated required-context promotion) — but the Coverage half landed
alone (`coverage-required-context`, applied 2026-06-14); the **Sanitizer** half
of option-A was never done.

Backlog: `docs/self-improvement/categories/tooling.md` → `sanitizer-required-context` (P1).

## Constraints

- **No Source/Core touch** → no Perf-gate section required (this plan is
  tests + CI-config + docs only).
- A required context that is **conditionally skipped** (`if: source_core_cpp`)
  would deadlock docs-only-PR merges on a never-run required check → both
  Sanitizer jobs MUST self-gate (emit the context green on the skip path) BEFORE
  promotion. This is the `# ci-required-context: self-gated` pattern Coverage
  already uses.
- **Maintainer-only bind**: the merge-gates poller enforces config
  `required_contexts` immediately, but GitHub-native branch protection only after
  the maintainer runs `agents/scripts/core/setup-branch-protection.sh` (loops
  `required_contexts` from config — no edit). The agent MUST NOT run it.
- **Dependency / sequencing**: a required ASAN lane that false-reds on
  ASAN-unsafe *fixtures* would deadlock real merges. The deep-nest ADF fixture
  (`adf-deep-nest-fixture-asan-unsafe`, test.md) overflows ASAN's inflated stack
  frames on `nlohmann::json`'s recursive destructor → must be hardened FIRST and
  land on develop BEFORE the config-flip PR. (The #1215 CallstackParser ReDoS
  ASan-timing wrap is already merged.)

## Slices

### Slice 1 — ADF deep-nest fixture hardening (test-only PR; lands first)

`tests/Core/TrackerFieldValueParser.extended.test.cpp` — the three hostile-deep-nest
TEST_CASEs build a `kDeepAdfDepth=400` json tree (> the 256 walker cap, so the cap
triggers). The walkers are depth-capped, but nlohmann's **destructor** is recursive
and overflows under ASAN on teardown — a fixture-only fragility, not a heap finding.
Fix = depth-independent **iterative** teardown so the fixture can nest past the cap
safely:

- add `#include <vector>`;
- add a generic `DismantleDeepJson(nlohmann::json&)` helper (worklist-based: pop a
  node, move its structured children onto the worklist, let the node destruct
  holding only moved-from/null children → depth 1);
- call it at the end of all three deep-nest TEST_CASEs (`doc` / `comments` / `deep`);
- rewrite the stale `kDeepAdfDepth` / per-test comments that claimed the depth was
  "kept modest so the recursive ctor/dtor stays safe".

**CI note**: `source_core_cpp` keys on `Source/Core|Plugins` C++ — a tests-only PR
does NOT match, so the per-PR ASAN lane **skips**. Verify LOCALLY under
`ninja-msvc-asan` (`-DSMATCHET_BUILD_TESTS=ON`) instead.

### Slice 1b — wall-clock timing-assertion ASAN guards (test-only PR; complementary to Slice 1)

The now-live required ASAN rig runs the whole doctest exe (minus the one excluded
CallstackParser case), so EVERY wall-clock `CHECK(elapsed < budget)` that ASAN
inflates ~3–10× can false-red the required lane on a slow runner → real-merge
deadlock (escapable only by the manual `sanitizer-out-of-band` label). An exhaustive
audit found **9 such assertions across 6 files**. Apply the #1215 guard pattern
(`#if defined(__SANITIZE_ADDRESS__)` → loosen ~10×; `#else` unchanged) to all 9
uniformly. The `UserInfoActivityCancelUaf` site guards ONLY the teardown-time budget —
the UAF sentinel / ASan heap-tracking asserts stay untouched. Separate branch
`feat/asan-timing-guards` off origin/develop; test-only; NOT watcher-registered.

### Slice 2 — ~~config-flip + self-gate~~ **SUPERSEDED by PR #1253 (Slice C)**

**DONE + LIVE — do NOT build.** Slice C (`coverage-sanitizer-required-contexts.md`)
promoted both Sanitizer lanes to live required contexts via PR #1253 and the Phase-2
`setup-branch-protection.sh` flip (live ruleset = 9 contexts, both sanitizers
present). It used **Pattern C** (job-level `if: source_core_cpp` skip; GitHub treats a
skipped required job as success) — so the Pattern-A `# ci-required-context: self-gated`
marker this slice had drafted is unnecessary AND incorrect for these jobs. The parity
gate passes because Pattern C carries no workflow-level `on.pull_request.paths` filter.
No config / workflow edits from this plan.

### Side task — down-scope `bucket-ui-lane-out-of-band-label` (docs)

#1259 (merged 2026-06-15) dropped `Bucket-*` from the poller block-list, so the
proposed `bucket-out-of-band` **downgrade** label is moot (nothing left to
downgrade). Rewrite the tooling.md entry: drop the downgrade-label half, keep only
the general raw-PUT guard as a residual P3; P2→P3; Last-reviewed 2026-06-15.
(Bundled here as a docs-only change; lands with Slice 1's docs or standalone.)

## Files to modify

| File | Slice | Change |
|---|---|---|
| `tests/Core/TrackerFieldValueParser.extended.test.cpp` | 1 | `<vector>`, `DismantleDeepJson`, 3 teardown calls, comment rewrite |
| `docs/self-improvement/categories/tooling.md` | side | down-scope bucket entry |
| `tests/Core/LocalCacheManagerChat.test.cpp` | 1b | ASAN-guard `perIterUs` budget (1 site) |
| `tests/Core/BulkImportAbandonNonBlocking.test.cpp` | 1b | ASAN-guard `abandonMs` + `joinMs` (2 sites) |
| `tests/Core/CancelToken.test.cpp` | 1b | ASAN-guard `elapsedMs` (1 site) |
| `tests/Core/SubprocessCapture.test.cpp` | 1b | ASAN-guard `durationMs` (2 sites) |
| `tests/Core/StubAiClientCancel.test.cpp` | 1b | ASAN-guard `postCancelMs` + `totalMs` (2 sites) |
| `tests/Core/UserInfoActivityCancelUaf.test.cpp` | 1b | ASAN-guard `teardownMs` ONLY (1 site; UAF asserts untouched) |
| ~~`.github/workflows/build-and-test.yml`~~ | ~~2~~ | **SUPERSEDED — Slice C / PR #1253 (Pattern C, no self-gate)** |
| ~~`project.config.json`~~ | ~~2~~ | **SUPERSEDED — already on develop + live (9 contexts)** |

## Verification

- Slice 1: `cmake --build --preset ninja-msvc-asan --target SmatchetTests` (through
  `scripts/dev/with-msvc-env.sh`) then run the doctest binary filtered to the three
  ADF deep-nest cases — must pass with NO ASAN stack-overflow. Lint:
  `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop`.
- Slice 1b: fresh `ninja-msvc-asan` build, run the full rig (minus the excluded
  CallstackParser case) — all 9 guarded sites pass under ASAN (no timing false-red);
  lint PASS. Non-ASAN budgets unchanged (the `#else` branch). **DONE** — full rig
  **1872 passed | 0 failed** under ASAN; lint PASS; guards PR #1280 (test-only).
- Slice 2: N/A — superseded; live ruleset already shows 9 required contexts.

## Implementation log

- 2026-06-15 — Slice 1 fixture edits applied (`<vector>`, `DismantleDeepJson`, 3
  teardown calls, comment rewrite); bucket entry down-scoped. ASAN build verified:
  3 deep-nest cases `3 | 3 passed | 0 failed` under `ninja-msvc-asan`, no
  stack-overflow; lint all PASS. Slice 1 shipped as PR #1273; registered with
  merge-watcher (auto-merge when green — authorized: test + docs only).
- 2026-06-15 — **Re-scope.** Recon found Slice C (PR #1253 + Phase-2 flip) already
  promoted both Sanitizer lanes to live required contexts (live ruleset = 9), using
  Pattern C not the Pattern-A self-gate this plan drafted → **Slice 2 dropped as
  superseded** (see banner + Slice 2 section). Plan re-scoped to ASAN-lane fragility
  hardening. User-approved (AskUserQuestion: "Ship the hardening").
- 2026-06-15 — **Slice 1 test work superseded by #1277 (duplicate-effort collision #2).**
  A parallel session's PR #1277 (`test(tracker): make ADF deep-nest fixture ASAN-safe via
  iterative teardown`) landed the IDENTICAL hardening on develop — same `DismantleDeepJson`
  helper, same `kDeepAdfDepth=400`, same iterative approach (trunk used the cleaner
  `DismantleDeepJson(json&&)` rvalue-ref API). Surfaced when #1273 went `DIRTY` after #1277
  merged; resolved the merge by taking trunk's version of
  `tests/Core/TrackerFieldValueParser.extended.test.cpp` wholesale (don't re-assert the
  lvalue-ref variant over a merged sibling) → #1273's test-file diff vs develop is now
  **empty**. #1273 still ships its docs (bucket-oob down-scope + this plan + the dup-preflight
  process lesson). Slice 1's *goal* (ASAN-safe ADF fixture on develop) is **achieved** — via
  #1277, not #1273. Second collision in this plan (after Slice 2 vs #1253) → broadened the
  `slice-dup-preflight` process lesson from "CI-config slice" to "any slice on a
  high-contention file/symbol" (categories/process.md Recurrence).
- 2026-06-15 — **Post-ship.** User authorized registering guards PR #1280 with the
  merge-watcher (post-ship menu → "Register #1280 too"); #1280 now auto-merges when green
  alongside #1273. Registration was a no-op — #1280 was already present in the watcher
  registry (`registered_at` predates the authorization, head `af508448`, 6 CR-none-grace
  polls; never merged — stayed BLOCKED awaiting checks); surfaced to user. Both PRs
  BLOCKED (normal, awaiting required checks) at hand-off.
- 2026-06-15 — Slice 1b: exhaustive audit found 9 wall-clock timing assertions across
  6 files; applied the #1215 `__SANITIZE_ADDRESS__` guard to all 9 uniformly on
  branch `feat/asan-timing-guards`. Guards (ASAN budget / non-ASAN budget):
  `LocalCacheManagerChat` perIterUs 69400/6940; `BulkImportAbandonNonBlocking`
  abandonMs 2500/250 + joinMs **WARN-downgrade under ASAN** (a 10× loosen would mask a
  full-runtime inline-join regression, so the cooperative-bail join budget degrades to
  `WARN` not a loosened `CHECK`); `CancelToken` elapsedMs; `SubprocessCapture` durationMs
  ×2; `StubAiClientCancel` postCancelMs + totalMs; `UserInfoActivityCancelUaf` teardownMs
  ONLY (UAF sentinel `0xC0FFEE` + heap-tracking asserts untouched). **ASAN verify**: fresh
  `ninja-msvc-asan`, full rig **1872 passed | 0 failed** — the feared ADF deep-nest
  overflow did NOT materialise even though the guards branch lacks Slice 1's ADF teardown
  fix. Lint PASS. Shipped as guards PR #1280 (test-only; **NOT** watcher-registered —
  user reserved auto-merge authorization for PR #1273 only).

## Deviations

- **Residual finding (surfaced, not silently fixed) → became Slice 1b.** Verifying
  Slice 1 under local ASAN exposed a second ASAN-fragile assertion unrelated to the
  ADF fixture: the `LocalCacheManagerChat` hydration-latency microbench
  (`tests/Core/LocalCacheManagerChat.test.cpp:194`, `CHECK(perIterUs < 6940.0)`) is a
  wall-clock perf assertion not ASAN-guarded — fails LOCALLY under ASAN (11829.4
  us/iter ≈ 1.7× the 6.94 ms / 144 Hz budget) but passes on CI's faster hardware.
  Same class as #1215. Surfaced per loop-mode `in`; user chose to audit ALL timing
  CHECKs (found 9) and guard them uniformly → folded into **Slice 1b** above, not a
  Slice-2 blocker (Slice 2 is superseded). The required ASAN lane is already **live**,
  so this hardening protects real merges from slow-runner false-reds now, not later.
- **Slice 2 superseded mid-flight (duplicate-effort collision).** ~10 concurrent
  sessions share the integration tree; a parallel `Slice C` landed the identical
  config promotion (PR #1253) + live flip while this plan was mid-Slice-1. Lesson:
  before a CI-config slice, grep open PRs + recent develop log for the same
  `required_contexts` edit. Logged to self-improvement (process).

# Plan — `sanitizer-required-context`: promote both Sanitizer PR lanes to required

Slug: `sanitizer-required-context` · Owner: orchestrator · Started: 2026-06-15 · Loop-mode: `in`

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

### Slice 2 — config-flip + self-gate (separate PR; held for maintainer; lands AFTER Slice 1)

- `.github/workflows/build-and-test.yml` — self-gate both `sanitizer-asan` and
  `sanitizer-ubsan-pr` jobs: on the `source_core_cpp == false` skip path emit the
  context name as a green status, marked `# ci-required-context: self-gated`
  (mirror Coverage), so docs-only PRs don't deadlock.
- `project.config.json` — add `Sanitizer (ASAN via MSVC)` + `Sanitizer (UBSan via Clang)`
  to `branch_protection.required_contexts` + `ci.required_checks` (Coverage already there).
- Parity selftest (`test-required-context-parity.sh`) green.
- **Held, NOT watcher-registered** — the maintainer reviews, merges after Slice 1
  is on develop, then runs `setup-branch-protection.sh` to make the bind live.

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
| `.github/workflows/build-and-test.yml` | 2 | self-gate both Sanitizer jobs |
| `project.config.json` | 2 | add both Sanitizer lanes to required_contexts + ci.required_checks |

## Verification

- Slice 1: `cmake --build --preset ninja-msvc-asan --target SmatchetTests` (through
  `scripts/dev/with-msvc-env.sh`) then run the doctest binary filtered to the three
  ADF deep-nest cases — must pass with NO ASAN stack-overflow. Lint:
  `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop`.
- Slice 2: parity selftest + the build-and-test workflow self-gate emits the
  context green on a docs-only PR (no deadlock).

## Implementation log

- 2026-06-15 — Slice 1 fixture edits applied (`<vector>`, `DismantleDeepJson`, 3
  teardown calls, comment rewrite); bucket entry down-scoped. ASAN build verified:
  3 deep-nest cases `3 | 3 passed | 0 failed` under `ninja-msvc-asan`, no
  stack-overflow; lint all PASS. Slice 1 shipped as PR #1273 (test + docs only;
  NOT watcher-registered — no auto-merge authorization given).

## Deviations

- **Residual finding (surfaced, not silently fixed) — Slice 2 blocker decision.**
  Verifying Slice 1 under local ASAN exposed a *second* ASAN-fragile assertion
  unrelated to the ADF fixture: the `LocalCacheManagerChat` hydration-latency
  microbench (`tests/Core/LocalCacheManagerChat.test.cpp:194`,
  `CHECK(perIterUs < 6940.0)`) is a wall-clock perf assertion **not ASAN-guarded**.
  It fails LOCALLY under ASAN (11829.4 us/iter ≈ 1.7× the 6.94 ms / 144 Hz budget,
  ASAN slowdown) but passes on CI's faster hardware (CI ASAN lane is consistently
  green when it runs). Same fragility class as #1215 (CallstackParser ReDoS timing
  wrap). Once the ASAN lane is **required** (Slice 2), a single slow CI runner
  could red it and deadlock a real merge. **Decision needed before Slice 2**:
  ASAN-guard the microbench (skip / loosen the threshold under
  `__SANITIZE_ADDRESS__` like #1215) vs. accept as CI-green residual. Out of the
  approved plan's scope → escalated per loop-mode `in`.

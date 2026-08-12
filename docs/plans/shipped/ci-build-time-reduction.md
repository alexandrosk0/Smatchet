# Plan — CI build-time reduction (sccache + build-reuse + matrix rebalance)
<!-- plan-date: 2026-06-03 -->

> **Status**: shipped — archived 2026-06-06; post-ship sections populated and cited PRs merged (see § Implementation log).
>
> **Slug**: `ci-build-time-reduction` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

A code PR triggers **up to six cold full MSVC builds**, each on a separate fresh `windows-2022` runner, each recompiling the same tree under a slightly different preset:

| Build (PR, code change) | Workflow / job | Preset | Reuses an artefact? |
|---|---|---|---|
| iter `SmatchetStandalone` + bucket-A + lint gate | build-and-test `windows-msvc` | `ninja-iter-msvc` | — (uploads its exe) |
| iter-light + `SmatchetCore_DX12` dual-target | build-and-test `windows-msvc-light` | `ninja-iter-msvc` (light flags) | no |
| ui-test `SmatchetStandalone` (ImGui Test Engine) | build-and-test `bucket-e-ui-tests` | `ninja-ui-test-msvc` | **no — builds from scratch** |
| ASAN build + ctest | build-and-test `sanitizer-asan` (45-min cap) | `ninja-msvc-asan` | no |
| test build + ctest + coverage (advisory) | coverage.yml | `ninja-test-msvc` | no |
| iter `SmatchetStandalone` (perf paths only) | perf-pr-fast.yml | `ninja-iter-msvc` | **no — rebuilds the same binary `windows-msvc` already made** |

bucket-C already downloads `windows-msvc`'s exe (no build). The critical path is `changes → windows-msvc → max(bucket-e build, sanitizer-asan build)` — i.e. **two serial cold builds** because bucket-E and ASAN each rebuild after `windows-msvc`.

**Measured baseline** (build-and-test run 26887105069 + coverage run 26864632142 — single-sample, ±): Windows+MSVC **15.0m** → Bucket-E **9.3m** (serial; builds ui-test) · Windows-light+DX12 **10.5m** · Sanitizer-ASAN **4.6m** · Bucket-C **0.7m** (download+run) · Coverage **7m**. **Critical path ≈ 24 min** (windows-msvc 15 + bucket-e 9.3); **total ≈ 47 Windows-minutes/PR** (+~10-15m when perf-pr-fast triggers). Bucket-C's 0.7m confirms download+run is near-free — the model for bucket-E. **Projected steady-state** (warm sccache ~2-3×/build + build-reuse + rebalance): **critical path ~10 min, ~13 Windows-min** — ≈2.4× faster wall-clock, ≈3.5× less compute. **These are upper-bound targets, contingent on measured sccache hit-rate** (see Approach Phase 1: cross-preset sharing is partial — asan/light hash differently — so the multiplier rests mainly on cross-**run** same-preset caching). The first post-enable run primes the cache (no gain); savings land from the 2nd PR onward. Docs-only PRs are already ~0 (Pattern-C skip) — unchanged.

Two facts make most of this cheap to fix:
1. **The build system already supports `sccache`/`ccache`** — `CMakeLists.txt:47-54` auto-wraps the compiler when `find_program(NAMES ccache sccache)` succeeds (disable via `SMATCHET_NO_CCACHE=1`). **CI never installs sccache**, so every build compiles cold without the object cache that's already plumbed.
2. **bucket-E and perf-pr-fast rebuild binaries another job already produced** (`ui-test` from scratch; the *identical* `ninja-iter-msvc SmatchetStandalone` that `windows-msvc` uploads).

Already-good (leave): `concurrency: cancel-in-progress`; docs-only PRs skip the builds (Pattern C `changes` gate); perf-full + sanitizer-nightly + high-integrity-baseline/narrowing are cron/develop-post-merge, not per-PR.

**Intended outcome — one sentence:** after this lands, a code PR pays roughly **warm** MSVC builds instead of ~six cold ones — via sccache (already CMake-supported, just not installed in CI), build-once-upload-reuse for **perf** (bucket-E reuse deferred to a conditional Phase 2b — see Q4), and ASAN gated to Core-C++-delta PRs (still pre-merge — see Q5).

## Approach

Three phases, biggest-and-safest first.

**Phase 1 — sccache in CI (highest ROI; the build system is already ready).** Install `sccache` on the Windows runners and point a GitHub Actions cache at its store; the existing `CMakeLists.txt:47-54` auto-wrap then picks it up. This warms the **object** cache primarily **across runs** (same preset, PR N → PR N+1 — the dominant win) and **partially across presets** in a single run. **Cross-preset sharing is weaker than a naive "six builds share most TUs" read** — sccache hashes the full compile command, so: `ninja-msvc-asan` (`Debug` + `/fsanitize=address`) shares **nothing** with the `RelWithDebInfo` builds; the **light** build (`-DSMATCHET_WITH_AI/MCP/WHISPER=OFF`) changes preprocessor defines → most TUs hash differently from full `iter`; `test` / `ui-test` / `iter` differ only in `SMATCHET_BUILD_*` options → **partial** intra-run share. The headline gain is therefore cross-**run** caching (the 2nd-PR-onward effect below), not cross-preset; treat the projected compute multiplier as an upper bound contingent on measured hit-rate, not a guarantee. **Two confirmed prerequisites** (not optional — verified against the tree):
- **Debug-info format.** All four CI presets (`ninja-iter-msvc` / `ninja-test-msvc` / `ninja-ui-test-msvc` = RelWithDebInfo, `ninja-msvc-asan` = Debug — confirmed `CMakePresets.json:70,98,113,129`) leave `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` **unset** → CMake defaults to `ProgramDatabase` (`/Zi`, separate PDB) for these configs, which sccache **cannot** cache. Phase 1 must set `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded` (`/Z7`) on these builds — **gated to CI / compile-cache-present** so local dev keeps PDBs (Embedded bloats `.obj` size).
  - **CMP0141 gate (blocking sub-prereq — confirmed live).** The `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` abstraction is governed by policy **CMP0141**, introduced in CMake **3.25**. The repo declares `cmake_minimum_required(VERSION 3.24)` (`CMakeLists.txt:1`) → with CMP0141 unset the policy defaults **OLD** → the variable is **silently ignored** and CMake still emits `/Zi` via the legacy flag path. The runners ship CMake ≥3.30 (the version is present), but the policy must be flipped or the flip is a **no-op** — yielding the exact ~0% hit-rate the Risks section warns about. **Decision (locked):** sidestep the CMAKE_MSVC_DEBUG_INFORMATION_FORMAT abstraction entirely (CMP0141 irrelevant — no policy flip, no `cmake_minimum_required` bump) and inject `/Z7` directly, **gated CI-only**:

```cmake
if(DEFINED ENV{CI} AND MSVC)
    add_compile_options(/Z7)  # CI: embedded debug-info so sccache can cache objects (separate-PDB /Zi is uncacheable)
endif()
```

`$ENV{CI}` is read at **configure** time (a generator expression cannot read env at build time), and GitHub Actions sets `CI=true` automatically — so the flip is CI-scoped, not cache-present-scoped: local dev (cache or not) keeps the default `/Zi` separate PDB. Independent of sccache install — harmless if the cache is absent (just larger `.obj`, no cache). Rejected B (`cmake_policy(SET CMP0141 NEW)`) and C (bump min to 3.25): both could shift debug-info behaviour for non-CI MSVC builds, exactly what the CI-only gate avoids. **Verify** the compile DB shows `/Z7` and no `/Zi` on a CI build before trusting Phase-1 hit-rate numbers.
- **Tool choice.** The existing auto-wrap is `find_program(NAMES ccache sccache)` — it **prefers `ccache`** and wraps it with `CCACHE_*` env. ccache's MSVC support is poor; **sccache** is the one with solid `cl.exe` support. CI must install **sccache specifically** (not ccache). The `CCACHE_*` env in the launcher is inert to sccache (it reads `SCCACHE_*` / GHA env), so it works through the existing wrapper — but verify the hit-rate, and if needed add an sccache branch to the CMake block passing `SCCACHE_*`.

**Cache backend (REVISED during impl — see § Deviations):** **local-disk** — `mozilla-actions/sccache-action@v0.0.10` installs the binary, `SCCACHE_DIR` points sccache at a local dir, and a per-preset sha-keyed `actions/cache` persists that dir across runs. The originally-locked GHA-native backend (`SCCACHE_GHA_ENABLED=true`) **broke the build on first attempt**: sccache talks to the GH cache *service* at compile time, and a service error (400 / "services aren't available") fails **server startup** → every compile fails → a REQUIRED check goes red on a cache-service hiccup. Local-disk decouples build success from cache-service uptime (an outage just yields a cold cache). The existing `CMakeLists.txt:55-60` launcher invokes `sccache <compiler> <args>` as a wrapper either way — no CMake change beyond the Q1 `/Z7` line.

**10 GB cache-contention (accept-and-measure, locked):** GitHub Actions cache is **10 GB/repo, LRU-evicted, shared** with the 5 existing per-preset FetchContent `actions/cache` blocks (kept). An MSVC object cache across iter+test+ui-test+light+asan could be large and churn those. Decision: **enable across all sccache'd builds, then measure** — add a `sccache --show-stats` step to each build and watch FetchContent restore-hit logs over the first few PRs. Only scope sccache down (drop asan/light, or set a `SCCACHE_GHA` size cap) **if eviction actually shows up**; don't pre-optimize cache sizing blind.

This phase touches no job graph — it just makes every existing build faster.

**Phase 2 — build-once-upload-reuse (the originating ask, generalised).** Eliminate the rebuilds of binaries another job already made. **Phase 2 ships perf-reuse + Mesa cache only; the bucket-E build-move is deferred behind Phase-1 measurement (locked decision — see below).**
- **bucket-E build-move — DEFERRED, conditional.** The tempting move is: `windows-msvc` additionally builds + uploads the `ninja-ui-test-msvc` exe, **bucket-E** drops its build → download + run. But bucket-E is **already serial after `windows-msvc`** (`needs: windows-msvc`), and **Phase 1 already warms bucket-E's own ui-test build** (cross-run sccache) — so moving it *into* `windows-msvc` serializes a **3rd** build (test+iter+ui-test) inside the critical-path pole to save a now-cheap warm build on a job that runs after it anyway. Net-negative or marginal once warm. **Decision: do NOT move it in Phase 2.** Leave bucket-E building its own (now-warm) exe. Re-evaluate ONLY if, after Phase 1, warm bucket-E build time is still a material critical-path pole — which happens only if `SMATCHET_BUILD_UI_TESTS=ON` defines make ui-test share poorly with iter/test even cross-run (an empirical question Phase 1 answers). The move is a conditional Phase-2b, not baseline Phase 2.
- **perf-pr-fast reuse — DROPPED post-measurement (was the originating ask).** Two findings killed the ROI after Phase 1 shipped: (1) `perf-pr-fast.yml` is a **separate workflow**, not a job in `build-and-test.yml` — GitHub `needs:` can't cross workflows, so "perf `needs: windows-msvc`" is **not directly possible**; it would need cross-workflow plumbing (move perf into build-and-test, or `workflow_run`, or gh-api artifact correlation). (2) perf-pr-fast is **off the critical path** — it runs fully parallel today (~7 min own build) and finishes long before the critical path (`windows-msvc → bucket-E/asan` ~24 min). Reuse would save ~7 min of **compute** but **zero wall-clock**. Moderate cost, low value → **dropped** (maintainer decision, Phase-2 measurement round). The `/Z7`-perf-neutral analysis stands but is moot.
- **Mesa `opengl32.dll` cache — SHIPPED.** bucket-C + bucket-E each re-downloaded the ~30 MB mesa3d 7z every run; now cached via `actions/cache` keyed `mesa-opengl32-<MESA_VERSION>` (shared across both jobs), download skipped on hit. Zero serialization risk, no warmth dependency — the safe, unconditional half of Phase 2.

**Phase 3 — PR matrix rebalance (policy; maintainer decided).** Skip jobs on PRs that can't exercise them; hold coverage. Outcome after Q5: asan Core-delta-gated (still pre-merge), coverage unchanged. The per-PR jobs that are redundant with nightly twins on irrelevant diffs:
- `sanitizer-asan` runs **~4.6m measured** (run 26887105069 — the `45-min` figure elsewhere is the *timeout cap*, not runtime; the real poles are `windows-msvc` 15.0m then Bucket-E 9.3m). It has a `sanitizer-nightly` twin, is **not** in `required_checks`, and builds Debug (`ninja-msvc-asan`) → **zero** sccache share with the RelWithDebInfo builds, so Phase 1 can't speed it. **Decision (maintainer, locked): Core-delta gate** — keep asan **pre-merge** but run it only when the PR diff touches `Source/Core/**` C++ (extend the `changes` job with a `source_core_cpp` output; gate asan's `if:` on it; keep `sanitizer-nightly`). **Pillar-3 preserved**: pre-merge UB detection stays exactly where UB originates (Core C++); asan is merely skipped on docs/yaml/Standalone-only PRs that can't introduce Core UB. Saves ~4.6 Windows-min/PR on non-Core PRs **without** relaxing the pre-merge "sanitizer clean" guarantee — chosen over a full post-merge move once the saving was corrected from ~45m (cap) to ~4.6m (actual): too small to justify a Pillar-3 relaxation. (No ADR: the Core-delta gate is a one-line `if:` flip — trivially reversible, no safety trade — so it doesn't clear the hard-to-reverse ADR bar. The decision lives here + in the workflow `if:`.)
- `coverage` is **advisory** (`continue-on-error: true`, `coverage.yml:37`) yet a full Windows build every code PR. Option: move to develop-post-merge until it graduates to blocking. **Dependency: the `coverage-threshold-graduation` plan flips it to blocking — if that lands, it must stay on PR.** **Live caveat (confirmed):** `coverage.yml:5-8,37` declare an advisory soak `until 2026-05-30; flip to false after` — that deadline is **already past** (today 2026-06-03), so the graduation-to-blocking is **due/overdue**. Do **not** relocate coverage off-PR assuming it stays advisory; reconcile with `coverage-threshold-graduation` first — if it's about to (or already should) block, coverage **stays on PR** and P3 touches only `sanitizer-asan`. Sequence accordingly.
- Optional: larger Windows runner to parallelise ninja (cost trade).

These are **flagged, not pre-decided** — they trade pre-merge safety for speed; the maintainer picks per the human-control/cost policy.

## Files to modify

**Phase 1 — sccache:**
1. `.github/workflows/build-and-test.yml` (edit) — install **sccache** (not ccache) via `mozilla-actions/sccache-action` + `SCCACHE_GHA_ENABLED=true` (Q2: GHA-native backend) on `windows-msvc`, `windows-msvc-light`, `bucket-e`, `sanitizer-asan`. The CMake auto-wrap picks sccache off PATH; no `COMPILER_LAUNCHER` override needed. (bucket-e keeps building its own warm ui-test exe — Q4 deferred the build-move — so it gets sccache too. asan gets the action for consistency but shares little, being Debug.) Add a `sccache --show-stats` step per build for the cache-contention watch.
2. `.github/workflows/coverage.yml` + `.github/workflows/perf-pr-fast.yml` (edit) — same sccache install + cache.
3. `CMakePresets.json` and/or `CMakeLists.txt` (edit — **required**, confirmed) — set `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded` for the CI presets (all 4 are RelWithDebInfo/Debug + currently unset → default `/Zi`, uncacheable). Gate the flip to CI / compile-cache-present (e.g. a CI-only preset overlay, or `if(SMATCHET_CCACHE_PROGRAM) set(... Embedded)` in `CMakeLists.txt`) so local dev keeps separate PDBs. Confirm the `:47` auto-wrap fires under each Ninja preset (it does — generator matches).

**Phase 2 — build-reuse:**
4. `.github/workflows/build-and-test.yml` (edit) — **Phase 2 baseline:** cache Mesa `opengl32.dll` (bucket-C + bucket-E both re-download it today, `build-and-test.yml:293,360`). **Phase 2b (conditional, deferred):** only if Phase-1 measurement shows warm bucket-E build is still a critical-path pole — `windows-msvc`: add Configure+Build `ninja-ui-test-msvc` + upload `smatchet-ui-test-exe-${run_id}`; `bucket-e`: replace its build with a download of that artefact + run.
5. `.github/workflows/perf-pr-fast.yml` (edit) — `needs: windows-msvc`; replace the iter build with a download of `smatchet-exe-${run_id}`; run perf scenarios against it.

**Phase 3 — rebalance (each gated on a maintainer yes):**
6. `.github/workflows/build-and-test.yml` (edit) — `changes` job: add a `source_core_cpp` path-filter output (`Source/Core/**` `.cpp`/`.h`). `sanitizer-asan`: gate `if: needs.changes.outputs.source_core_cpp == 'true'` (Q5 = Core-delta gate; stays pre-merge, skips non-Core PRs; keep `sanitizer-nightly`).
7. `.github/workflows/coverage.yml` (edit) — **NO CHANGE in this plan.** Coverage stays on PR: its advisory soak deadline (`2026-05-30`) is already past and `coverage-threshold-graduation` (still in `docs/plans/active/`, unshipped) is due/overdue to flip it blocking — relocating it off-PR now would collide with that graduation. Revisit only after the graduation plan resolves.
8. **No config key, no ADR.** The asan Core-delta gate is recorded by its workflow `if:` (the executable source of truth) + this plan's Phase 3. A `project.config.json` `ci.job_schedule` key was **rejected** — dead data nothing reads, a drift-prone parallel copy of the YAML (the anti-pattern `test-portable-purity` guards). `ci.required_checks` already omits asan + coverage, so no config edit is needed.

## Existing utilities reused

- `CMakeLists.txt:47-54` ccache/sccache auto-wrap (+ `SMATCHET_NO_CCACHE`) — the build-side support Phase 1 turns on in CI; **no CMake change beyond the `/Z7` check**.
- `windows-msvc`'s existing `upload-artifact` (`smatchet-exe-${run_id}`) + bucket-C's `download-artifact` — the exact build-reuse pattern Phase 2 extends to **perf** (and, conditionally, ui-test in Phase 2b).
- The per-preset `actions/cache` FetchContent blocks — kept (sccache caches objects; FetchContent cache covers dep *sources*).
- `changes` job (Pattern C) + `concurrency.cancel-in-progress` — already minimise wasted runs; untouched.
- `sanitizer-nightly` + `perf-full` (cron) — the post-merge twins Phase 3 leans on when moving PR jobs off the critical path.
- `project.config.json` `ci` block — where the PR-vs-post-merge split is recorded.

## UX Pillar callouts

- **Pillar 1 (perf)**: no runtime impact; perf-pr-fast *gains* fidelity (measures the shared binary). Pillars 2–4: no impact — CI infra only.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

N/A — no Source/Core code. The diff is `.github/workflows/*.yml` + `CMakePresets.json` + `project.config.json`; `.github/**` is deny-listed by is-pure-docs-diff.sh so it is not pure-docs and `test-all.sh` runs, but no `.cpp`/`.h` changes → build/ctest/perf gates are no-ops. Verification is workflow-yaml lint + measured wall-clock before/after.

## Risks / non-goals

**Risks:**
- **sccache + MSVC PDB incompatibility (confirmed live).** All four CI presets build RelWithDebInfo/Debug with the debug-info format unset → CMake default `/Zi` (separate PDB), which sccache cannot cache. Without the `Embedded` flip, Phase 1 yields ~0% hit rate. → the flip is a **required** Phase-1 step (gated to CI so local keeps PDBs); a build that genuinely needs `/Zi` opts out via `SMATCHET_NO_CCACHE=1` rather than miscache. Also: the auto-wrap prefers `ccache` — installing `ccache` instead of `sccache` would wrap a tool with poor MSVC support and silently under-cache; CI installs sccache specifically.
- **sccache cache-poisoning / false hits** — wrong flags hashed → stale objects. → use the maintained `sccache` GHA action with proper hashing; a clean-build fallback (`SMATCHET_NO_CCACHE=1`) is one env var away; the existing FetchContent caches are unaffected.
- **Phase 2 serialises perf behind `windows-msvc`** (was a parallel cold build). → only adopt after Phase 1 makes `windows-msvc` sccache-fast; with a warm build, serial-download beats parallel-cold. Measure before committing Phase 2's perf change (Q3 sequencing gate). bucket-E is **not** serialised by Phase 2 — its build-move is deferred (Q4); it stays as-is (now warm via Phase-1 sccache).
- **Phase 3 does NOT reduce pre-merge safety** (Q5 corrected) — ASAN stays pre-merge via a Core-delta gate (ADR-0016): it still runs on every PR that touches `Source/Core/**` C++, only skipped on PRs that can't introduce Core UB. Pillar-3 pre-merge guarantee intact. A full post-merge move was rejected once the saving was corrected from ~45m (cap) to ~4.6m (actual). **Coverage is NOT moved** either — its graduation-to-blocking is due/overdue, so it stays on PR. Net: Phase 3 is a low-risk skip-on-irrelevant-PRs optimization, not a safety relaxation.
- **Artefact retention / size** — extra uploaded exe (`smatchet-ui-test-exe`). → `retention-days: 1` like the existing exe artefact; trivial size.

**Non-goals:**
- Changing what each gate *checks* — only *where/when/how-fast* it builds.
- Merging bucket-C + bucket-E into one job — rejected earlier (couples them, loses parallel isolation, still needs both binaries).
- Self-hosted runners — out of scope (larger GitHub runner noted as an option only).
- Touching the docs-only Pattern-C skip or the merge-gate set.

## Verification

- **Baseline (recorded)**: run 26887105069 — Windows+MSVC 15.0m, Bucket-E 9.3m, light+DX12 10.5m, Sanitizer-ASAN 4.6m, Bucket-C 0.7m; coverage run 26864632142 = 7m. Critical path ≈ 24m, ≈ 47 Windows-min/PR. **Target after P1+P2 (warm): critical path ≤ ~12m; after P3: ≤ ~14 Windows-min/PR.** Re-measure a code PR post-change against these.
- **Phase 1**: a second run on the same diff shows MSVC compile steps dropping to mostly sccache hits (sccache `--show-stats` in a step); total Windows-build wall-clock falls materially. **Compile DB shows `/Z7`, no `/Zi`** (Q1 flip actually applied). **Cache-contention watch**: FetchContent `actions/cache` restore-hit logs stay green across the first few PRs (no sccache↔FetchContent LRU eviction); if eviction shows, scope sccache down per the accept-and-measure clause.
- **Phase 2**: perf-pr-fast shows no `cmake --build` of the app (download `smatchet-exe-${run_id}` + run only); Mesa step is an `actions/cache` hit on both bucket-C and bucket-E. (bucket-E still builds its own warm ui-test exe — Q4; only Phase 2b would make it download.)
- **Correctness preserved**: bucket-C/E + sanitizer + coverage + perf all still pass on a known-good and fail on a seeded regression (no gate weakened, only relocated/sped).
- **Workflow lint**: `test-workflow-yaml.sh` green on every edited workflow.
- **Doc validation (blocks this plan PR)**: the `test-docs.sh` suite green — `test-portable-purity`, `test-plan-index`, `test-plan-ref-integrity`, `test-markdown-links`, `test-doc-anchors` (a red doc-validation job blocks merge even though non-required).
- **Plan stress-test**: run `grill-with-docs` on this plan before finalising (AGENTS.md § Plan-doc family) and record the outcome.
- **Build gate**: N/A — no compile in this diff.
- **Manual residue**: the Phase-3 PR-vs-post-merge choices are maintainer decisions, recorded in the `ci` config block — not silent.

## Out of scope (flagged, not designed)

**Deferral residue-sweep** (AGENTS.md § Process rules § Scope-reduction edits): before this plan finalises, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred below, and revise or delete them.

- **Larger / self-hosted Windows runners** — a parallelism lever; cost trade, separate decision.
- **Unifying the 5 per-preset FetchContent caches into one shared dep cache** — sccache covers dep *object* compilation; revisit only if dep *fetch* time dominates after Phase 1.
- **A hard CI-minute budget / cost ceiling** — pairs with the `ai-control-policy` cost-control clause; separate follow-up.

## Implementation log

- **Phase 1 (PR #796, MERGED)** — sccache enablement + Q1 `/Z7`. `CMakeLists.txt` CI-only `/Z7` via per-config flag string-replace (RelWithDebInfo+Debug), gated `if(DEFINED ENV{CI} AND MSVC)`. sccache binary via `mozilla-actions/sccache-action@v0.0.10` + local-disk backend on `windows-msvc`, `windows-msvc-light`, `bucket-e`, `sanitizer-asan` (build-and-test.yml), `coverage.yml`, `perf-pr-fast.yml`; per-job `sccache --show-stats` + a per-preset sha-keyed `actions/cache` for `$SCCACHE_DIR`. **Verified at CI**: the cl command line shows `/W4 /MP /Z7 /O2` with **no `/Zi`** — Q1 flip applies correctly; all MSVC builds green on the local-disk backend. First run is a cold-cache primer; cross-run speedup lands from the 2nd Core PR (confirmed: #799's warm run dropped windows-msvc 15m→11m, light →8m).
- **Phase 2 (PR #802, MERGED — Mesa cache only)** — cache the extracted Mesa `opengl32.dll` in bucket-C + bucket-E (`actions/cache`, key `mesa-opengl32-24.2.5`, download skipped on hit). **perf-pr-fast reuse DROPPED** post-measurement (separate workflow → no cross-workflow `needs:`; off critical path → compute-only ~7 min, zero wall-clock). bucket-E build-move (Phase 2b) remains conditional/deferred (Q4).
- **Phase 3 (PR #799 — asan Core-delta gate)** — `changes` job gains a `source_core_cpp` output (true when a `Source/Core/**` or `Source/Plugins/**` `.cpp/.h/.hpp/.cc/.cxx/.hh` changed; `case` globs span `/`; **fail-safe TRUE on empty/uncertain diff** so the sanitizer is never skipped when classification fails — CodeRabbit-flagged inversion fixed); `sanitizer-asan` gains `needs: [changes, windows-msvc]` + `if: needs.changes.outputs.source_core_cpp == 'true'`. asan stays pre-merge but skips Standalone-only / docs / yaml PRs (sanitizer-nightly still covers them). asan is not a required check, so the skip can't wedge branch protection. Classification unit-tested locally. **Note:** widened the plan's literal `Source/Core/**` to also include `Source/Plugins/**` — both are shared first-party C++ the asan build compiles where UB can originate; only `Source/Standalone/**` is the excluded first-party C++ (matches the plan's "Standalone-only PRs" exclusion intent).
- **Dep-fetch resilience (PRs #840 + #843, MERGED — out-of-band follow-up).** A `lua.org` outage red-walled `develop` for hours: Lua 5.3.6 is the one dependency fetched over plain HTTP (`file(DOWNLOAD)`; every other dep is an immutable git ref), so an upstream timeout fails the configure step on all four MSVC builds. **#840** made the fetch resilient — primary github-hosted mirror (byte-identical canonical tarball on `origin/bug-report-assets:deps/`), `lua.org` fallback, 3× retry, post-download SHA256 verify (not `file(DOWNLOAD EXPECTED_HASH)`, which hard-errors and blocks fallback). **#843** fixed a latent bug this exposed: the 5 "kept" per-preset FetchContent `actions/cache` blocks (§ Existing utilities reused) cached `build/<preset>/_deps`, but `CMakeLists.txt:269` overrides `FETCHCONTENT_BASE_DIR` to `${CMAKE_SOURCE_DIR}/.fetchcontent-src` — so the cached path was **empty** and every build re-fetched all deps from upstream (which is *why* the outage could red-wall a repo that nominally has a dep cache). Pointed all four blocks at `.fetchcontent-src`; after the first green build the Lua tarball + all git-ref deps now restore from cache → builds immune to any single upstream-dep outage. Follow-ups filed: a CI assertion that the cache path tracks `FETCHCONTENT_BASE_DIR`, and migrating Lua off plain HTTP (categories/infra.md, 2026-06-04).

## Deviations from plan

- **Q2 backend GHA-native → local-disk (forced by a build break).** The plan locked GHA-native (`SCCACHE_GHA_ENABLED`). First CI attempt failed every MSVC compile: `sccache: error: Server startup failed: cache storage failed to read ... status: 400 ... service: ghac`. The GHA-native backend couples each compile to the GH cache *service*; a service error fails sccache **server startup** = fatal build failure on a REQUIRED check. Switched to local-disk (`SCCACHE_DIR` + per-preset sha-keyed `actions/cache`), which the grill had considered as Q2-option-2 and the plan's Risks section had wanted as "a clean-build fallback one env var away". Structurally robust: cache-service outage → cold cache, never a red required check. Also bumped the action `v0.0.6 → v0.0.10`. The cross-run caching win (the grill's identified dominant gain) is unaffected by the backend choice.
- **D9025 averted.** `/Z7` applied via flag string-replace (not `add_compile_options`) specifically to avoid the "overriding /Zi with /Z7" warning that `test-build-warnings.sh` would fail — confirmed clean at CI (build reached link/compile, no D9025).

## Verification (actual)

- **sccache warmth (measured, run 26895709973 `windows-msvc`, warm off develop's `99da3feb` cache):** Compile requests **954**, executed 722, **Cache hits 469 (64.96%)**, misses 253, non-cacheable 232. So sccache **works** — ~65% hit rate, cross-run. **But wall-clock gain is modest** (test-build 321→272s, iter 375→360s ≈ 15%), because the uncached critical-path parts dominate: configure/FetchContent **171s** (124+47), linking, and the heavy cache-miss TUs (Amdahl — hits skew to cheap files). The grill's hedge ("upper-bound, contingent on hit-rate; mainly cross-run, not the 2-3×") was correct. **Phase 1 is the dominant win and it is real but moderate (~15-20% on build steps), not the headline 2.4×/3.5×.** The 2.4×/3.5× projection assumed Phase 2 build-reuse on the critical path; with perf-reuse dropped (off-critical-path) and bucket-E build-move deferred, the realized critical-path gain is the sccache build-step shave only.
- **Q1 `/Z7` applied** — confirmed in the CI cl command line (`/W4 /MP /Z7 /O2`, no `/Zi`).
- **Phase 2 Mesa cache** — `actions/cache` hit skips the ~30 MB mesa3d download in bucket-C + bucket-E.
- **Phase 3 asan Core-delta** — classification unit-tested locally (8 cases); skips asan on non-Core/Plugins PRs (verified the yaml-only Phase-3 PR itself skips asan).
- **Outstanding:** workflow-yaml lint green on every edited workflow (13/0). No `cmake`/`ctest`/perf gate — `.github/**` diff, no `.cpp`/`.h`.

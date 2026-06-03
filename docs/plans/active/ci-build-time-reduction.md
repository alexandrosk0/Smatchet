# Plan — CI build-time reduction (sccache + build-reuse + matrix rebalance)

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

**Measured baseline** (build-and-test run 26887105069 + coverage run 26864632142 — single-sample, ±): Windows+MSVC **15.0m** → Bucket-E **9.3m** (serial; builds ui-test) · Windows-light+DX12 **10.5m** · Sanitizer-ASAN **4.6m** · Bucket-C **0.7m** (download+run) · Coverage **7m**. **Critical path ≈ 24 min** (windows-msvc 15 + bucket-e 9.3); **total ≈ 47 Windows-minutes/PR** (+~10-15m when perf-pr-fast triggers). Bucket-C's 0.7m confirms download+run is near-free — the model for bucket-E. **Projected steady-state** (warm sccache ~2-3×/build + build-reuse + rebalance): **critical path ~10 min, ~13 Windows-min** — ≈2.4× faster wall-clock, ≈3.5× less compute. The first post-enable run primes the cache (no gain); savings land from the 2nd PR onward. Docs-only PRs are already ~0 (Pattern-C skip) — unchanged.

Two facts make most of this cheap to fix:
1. **The build system already supports `sccache`/`ccache`** — `CMakeLists.txt:47-54` auto-wraps the compiler when `find_program(NAMES ccache sccache)` succeeds (disable via `SMATCHET_NO_CCACHE=1`). **CI never installs sccache**, so every build compiles cold without the object cache that's already plumbed.
2. **bucket-E and perf-pr-fast rebuild binaries another job already produced** (`ui-test` from scratch; the *identical* `ninja-iter-msvc SmatchetStandalone` that `windows-msvc` uploads).

Already-good (leave): `concurrency: cancel-in-progress`; docs-only PRs skip the builds (Pattern C `changes` gate); perf-full + sanitizer-nightly + high-integrity-baseline/narrowing are cron/develop-post-merge, not per-PR.

**Intended outcome — one sentence:** after this lands, a code PR pays roughly **one** warm MSVC build instead of ~six cold ones — via sccache (already CMake-supported, just not installed in CI), build-once-upload-reuse for bucket-E + perf, and an explicitly-decided PR-vs-post-merge split for the heavy advisory jobs.

## Approach

Three phases, biggest-and-safest first.

**Phase 1 — sccache in CI (highest ROI; the build system is already ready).** Install `sccache` on the Windows runners and point a GitHub Actions cache at its store; the existing `CMakeLists.txt:47-54` auto-wrap then picks it up. This warms the **object** cache across runs *and* across presets (the six builds share most TUs), turning cold compiles into cache hits. **Two confirmed prerequisites** (not optional — verified against the tree):
- **Debug-info format.** All four CI presets (`ninja-iter-msvc` / `ninja-test-msvc` / `ninja-ui-test-msvc` = RelWithDebInfo, `ninja-msvc-asan` = Debug) leave `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` **unset** → CMake defaults to `ProgramDatabase` (`/Zi`, separate PDB) for these configs, which sccache **cannot** cache. Phase 1 must set `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded` (`/Z7`) on these builds — **gated to CI / compile-cache-present** so local dev keeps PDBs (Embedded bloats `.obj` size).
- **Tool choice.** The existing auto-wrap is `find_program(NAMES ccache sccache)` — it **prefers `ccache`** and wraps it with `CCACHE_*` env. ccache's MSVC support is poor; **sccache** is the one with solid `cl.exe` support. CI must install **sccache specifically** (not ccache). The `CCACHE_*` env in the launcher is inert to sccache (it reads `SCCACHE_*` / GHA env), so it works through the existing wrapper — but verify the hit-rate, and if needed add an sccache branch to the CMake block passing `SCCACHE_*`.

This phase touches no job graph — it just makes every existing build faster.

**Phase 2 — build-once-upload-reuse (the originating ask, generalised).** Eliminate the rebuilds of binaries another job already made:
- `windows-msvc` additionally builds + uploads the `ninja-ui-test-msvc` exe; **bucket-E** drops its from-scratch build → download + run (removes a serial cold build from the critical path).
- **perf-pr-fast** downloads `windows-msvc`'s `ninja-iter-msvc` exe (it builds the identical preset/target) instead of rebuilding — also tightens perf *fidelity* (measures the exact same binary). Best **after** Phase 1, since this makes perf-pr-fast `needs: windows-msvc` (serial) rather than a parallel cold build — only a win once `windows-msvc` is sccache-fast.
- Cache the Mesa `opengl32.dll` 7z (bucket-C + bucket-E each re-download it today).

**Phase 3 — PR-vs-post-merge matrix rebalance (policy; maintainer decides each).** The heaviest per-PR jobs are partly redundant with post-merge/nightly coverage:
- `sanitizer-asan` (the 45-min pole) already has a `sanitizer-nightly` twin. Option: run per-PR ASAN only on `Source/Core` C++ deltas (it already skips docs via `needs: windows-msvc`), or move it to develop-post-merge + nightly. **Trade: catches UB post-merge instead of pre-merge.**
- `coverage` is **advisory** (continue-on-error) yet a full Windows build every code PR. Option: move to develop-post-merge until it graduates to blocking. **Dependency: the `coverage-threshold-graduation` plan flips it to blocking — if that lands, it must stay on PR.** Sequence accordingly.
- Optional: larger Windows runner to parallelise ninja (cost trade).

These are **flagged, not pre-decided** — they trade pre-merge safety for speed; the maintainer picks per the human-control/cost policy.

## Files to modify

**Phase 1 — sccache:**
1. `.github/workflows/build-and-test.yml` (edit) — install **sccache** (not ccache) + an `actions/cache` (or `SCCACHE_GHA_ENABLED=true` via the `mozilla-actions/sccache-action`) on `windows-msvc`, `windows-msvc-light`, `bucket-e`*, `sanitizer-asan`. The CMake auto-wrap picks sccache off PATH; no `COMPILER_LAUNCHER` override needed. (*bucket-e only if Phase 2 keeps a build there.)
2. `.github/workflows/coverage.yml` + `.github/workflows/perf-pr-fast.yml` (edit) — same sccache install + cache.
3. `CMakePresets.json` and/or `CMakeLists.txt` (edit — **required**, confirmed) — set `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded` for the CI presets (all 4 are RelWithDebInfo/Debug + currently unset → default `/Zi`, uncacheable). Gate the flip to CI / compile-cache-present (e.g. a CI-only preset overlay, or `if(SMATCHET_CCACHE_PROGRAM) set(... Embedded)` in `CMakeLists.txt`) so local dev keeps separate PDBs. Confirm the `:47` auto-wrap fires under each Ninja preset (it does — generator matches).

**Phase 2 — build-reuse:**
4. `.github/workflows/build-and-test.yml` (edit) — `windows-msvc`: add Configure+Build `ninja-ui-test-msvc` + upload `smatchet-ui-test-exe-${run_id}`; `bucket-e`: replace its build with a download of that artefact + run. Cache Mesa `opengl32.dll`.
5. `.github/workflows/perf-pr-fast.yml` (edit) — `needs: windows-msvc`; replace the iter build with a download of `smatchet-exe-${run_id}`; run perf scenarios against it.

**Phase 3 — rebalance (each gated on a maintainer yes):**
6. `.github/workflows/build-and-test.yml` (edit) — `sanitizer-asan`: add a `Source/Core` C++-delta condition or an `if: github.event_name == 'push'` develop-post-merge gate (chosen value per maintainer).
7. `.github/workflows/coverage.yml` (edit) — develop-post-merge gate **unless** `coverage-threshold-graduation` has made it blocking.
8. `docs/agent-rules/` or `project.config.json` `ci` block (edit) — record the PR-vs-post-merge split decision so it's documented, not just YAML.

## Existing utilities reused

- `CMakeLists.txt:47-54` ccache/sccache auto-wrap (+ `SMATCHET_NO_CCACHE`) — the build-side support Phase 1 turns on in CI; **no CMake change beyond the `/Z7` check**.
- `windows-msvc`'s existing `upload-artifact` (`smatchet-exe-${run_id}`) + bucket-C's `download-artifact` — the exact build-reuse pattern Phase 2 extends to ui-test + perf.
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
- **Phase 2 serialises perf/bucket-E behind `windows-msvc`** (were parallel cold builds). → only adopt after Phase 1 makes `windows-msvc` sccache-fast; with a warm build, serial-download beats parallel-cold. Measure before committing Phase 2's perf change.
- **Phase 3 reduces pre-merge safety** (ASAN/coverage post-merge catch issues after merge). → **not pre-decided** — flagged for the maintainer per the cost-vs-quality policy; the develop-post-merge + nightly twins still catch them, just later. Coverage move is **blocked** if `coverage-threshold-graduation` makes it a required gate.
- **Artefact retention / size** — extra uploaded exe (`smatchet-ui-test-exe`). → `retention-days: 1` like the existing exe artefact; trivial size.

**Non-goals:**
- Changing what each gate *checks* — only *where/when/how-fast* it builds.
- Merging bucket-C + bucket-E into one job — rejected earlier (couples them, loses parallel isolation, still needs both binaries).
- Self-hosted runners — out of scope (larger GitHub runner noted as an option only).
- Touching the docs-only Pattern-C skip or the merge-gate set.

## Verification

- **Baseline (recorded)**: run 26887105069 — Windows+MSVC 15.0m, Bucket-E 9.3m, light+DX12 10.5m, Sanitizer-ASAN 4.6m, Bucket-C 0.7m; coverage run 26864632142 = 7m. Critical path ≈ 24m, ≈ 47 Windows-min/PR. **Target after P1+P2 (warm): critical path ≤ ~12m; after P3: ≤ ~14 Windows-min/PR.** Re-measure a code PR post-change against these.
- **Phase 1**: a second run on the same diff shows MSVC compile steps dropping to mostly sccache hits (sccache `--show-stats` in a step); total Windows-build wall-clock falls materially.
- **Phase 2**: bucket-E + perf-pr-fast show no `cmake --build` of the app (download + run only); the uploaded ui-test exe is consumed; Mesa step is a cache hit.
- **Correctness preserved**: bucket-C/E + sanitizer + coverage + perf all still pass on a known-good and fail on a seeded regression (no gate weakened, only relocated/sped).
- **Workflow lint**: `test-workflow-yaml.sh` green on every edited workflow.
- **Build gate**: N/A — no compile in this diff.
- **Manual residue**: the Phase-3 PR-vs-post-merge choices are maintainer decisions, recorded in the `ci` config block — not silent.

## Out of scope (flagged, not designed)

- **Larger / self-hosted Windows runners** — a parallelism lever; cost trade, separate decision.
- **Unifying the 5 per-preset FetchContent caches into one shared dep cache** — sccache covers dep *object* compilation; revisit only if dep *fetch* time dominates after Phase 1.
- **A hard CI-minute budget / cost ceiling** — pairs with the `ai-control-policy` cost-control clause; separate follow-up.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

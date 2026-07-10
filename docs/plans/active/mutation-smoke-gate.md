# Plan — Mutation-smoke gate (testing-surface-roadmap Slice F)

> **Slug**: `mutation-smoke-gate` (matches this file's basename without `.md`).
>
> **Status**: `active` — **Phases 1 + 2 shipped in this PR.** Phase 1 = harness + seed corpus (dry-run-validated). Phase 2 = advisory nightly wiring + bats + local mirror, per the owner's decisions (2026-07-10): **advisory nightly → graduate to blocking after clean runs**, **10-mutant seed @ 80% floor** to start. Phase 3 (corpus expansion to the full 17-TU set) and Phase 4 (blocking graduation) remain future work — the gate is `continue-on-error` until then, so it can never block a nightly.
>
> **Roadmap**: [`testing-surface-roadmap.md`](testing-surface-roadmap.md) Slice **F** (Gap 4, "Mutation-smoke / coverage-delta gate"). **Precursor**: [`MUTATION_PILOT.md`](../../../MUTATION_PILOT.md) (2026-07-05). **Originating backlog**: `docs/self-improvement/categories/tooling/2026-07-05-mutation-harness-slice-f.md`.

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

## Deviations from plan

- None yet.

## Verification (actual)

- Phase 1: shellcheck clean; corpus valid JSON; `--dry-run` green with tree clean afterward (see § Verification).

## Archive (post-ship — DO IN THIS PR, never a follow-up)

- On the slice completing (Phase 4 blocking graduation), flip `docs/self-improvement/categories/tooling/2026-07-05-mutation-harness-slice-f.md` → `applied`, archive to `applied.md`, and move this plan to `docs/plans/shipped/`.

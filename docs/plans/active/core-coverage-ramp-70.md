# Plan — Core coverage ramp 65 → 70 (PR-14)

> **Slug**: `core-coverage-ramp-70` (matches this file's basename without `.md`).
>
> **Status**: `active` — picked up from `docs/plans/backlog-pr-roadmap.md` PR-14. Two backlog members: `raise-core-coverage-67-to-70` (test.md) + `backend-impl-coverage-recovery` (debt.md). **Environment note**: the *authoritative* coverage number comes from the Windows CI `coverage.yml` job (OpenCppCoverage). The final threshold flip must be verified there, not from a local Linux `lcov+gcov` fallback (different exclusion set → non-comparable number).

## Context

The numeric line-coverage gate graduated advisory→blocking on 2026-06-04 (`docs/plans/coverage-threshold-graduation.md`), but the first real OpenCppCoverage measurement on the Ui-excluded `Source/Core` surface was **67%** — below the 70% end-state target. So the gate shipped at a **floor of 65** (`--threshold 65` in `coverage.yml`; `threshold: 65` in `project.config.json`) with a raise-to-70 ramp owed. Separately, PR #939 linked 5 real `JiraClient` impl TUs into `SmatchetTests` (a vtable requirement for the catalog-build fixture); absolute coverage rose but the line-rate dropped, needing the `coverage-out-of-band` label class (#941).

Intended outcome: *after this lands, measured `Source/Core` line coverage clears 70% with headroom, the gate threshold is 70 in both config sources, and the `coverage-out-of-band` label class is retired.*

## Approach

Two-phase, test-first. **Phase A — add tests to the biggest gaps.** Read the per-package `line-rate` from a `coverage.yml` run's `coverage.xml` artifact to target the lowest-covered strict-zone units, and author `test-rig` unit tests until the global measured rate clears 70% with a stable ≥2pt headroom. **Phase B — flip the threshold** (one line each) in `project.config.json` § coverage `threshold` 65→70 and `coverage.yml` `--threshold 65`→`70`, and remove `coverage-out-of-band` from the class once the backend-impl surface clears the floor.

The backend-impl slice (`backend-impl-coverage-recovery`) is the highest-leverage Phase-A target: extend the scripted-HTTP fixture pattern in `tests/support/JiraCatalogHttpFixture.h` to the search / mutation / user-meta paths of `JiraClient` (the catalog fixture currently exercises catalog endpoints only), then to Plane/GitHub impl TUs as they get linked. Trade-off: the threshold flip is gated on a *measured* number from the Windows job — do not flip on a projected or locally-estimated figure, or the gate red-bars `develop`.

## Files to modify

1. `tests/support/JiraCatalogHttpFixture.h` — extend the scripted-httplib fixture to cover search/mutation/user-meta request paths (not just catalog endpoints).
2. `tests/Core/*.test.cpp` (new, per lowest-covered unit) — `test-rig`-authored cases on the units the `coverage.xml` artifact flags as the biggest gaps; register each in `tests/CMakeLists.txt` (source + test TU, mirroring the existing `Core/*Pure.test.cpp` rows).
3. `project.config.json` — § coverage `threshold` 65 → 70 (Phase B only, after measured ≥70).
4. `.github/workflows/coverage.yml` — `--threshold 65` → `70` at the blocking capture step (Phase B only).
5. `docs/plans/coverage-threshold-graduation.md` — one-line § Implementation-log append recording the flip date + measured rate (PR-only edit of a shipped plan).

## Existing utilities reused

- `JiraCatalogHttpFixture` — `tests/support/JiraCatalogHttpFixture.h` — the scripted-HTTP fixture pattern the backend-impl slice extends.
- `scripts/dev/coverage.sh` — OpenCppCoverage (Windows) `--threshold N` runner; the authoritative measurement.
- `.github/workflows/coverage.yml` — the blocking gate whose threshold Phase B raises.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — this plan adds test TUs and flips two config numbers; it extracts/splits nothing.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — test-only + config; no product-runtime path changes.
- **Pillar 2 (UI-thread)**: no impact (test/config only).
- **Pillar 3 (never crash)**: no impact; new tests exercise existing code paths without changing them.
- **Pillar 4 (accessibility)**: no impact.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A`)

`N/A` — the diff touches `tests/`, `project.config.json`, and `.github/workflows/`; no `Source/Core/src/` runtime code changes. Each new test only *exercises* existing units.

## Risks / non-goals

- **Risk: flipping the threshold on a non-authoritative number red-bars `develop`.** Mitigation: Phase B flips only after the Windows `coverage.yml` job measures ≥70 with ≥2pt headroom; never flip on a projected/local figure.
- **Risk: added tests raise absolute coverage but not the line-rate (the #939 trap — linking more impl TUs dilutes the denominator).** Mitigation: target the fixture at the newly-linked backend-impl surface specifically, not just easy pure units.
- **Non-goal: per-file ≥90% high-risk-unit gate.** That's the separate P3 ramp (test.md); out of scope here.
- **Non-goal: excluding more UI files from the surface.** The Ui-exclusion set is fixed by `coverage-threshold-graduation`; this plan raises coverage of the *testable* surface, not the exclusion boundary.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: the new `tests/Core/*.test.cpp` green under `cmake --build --preset ninja-test-msvc && ctest --output-on-failure`.
- **Bucket E**: N/A — no UI/screenshot surface.
- **Bash-driver scenario / screenshot / sanitizer**: N/A.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — confirms the new fixture/test TUs don't break the app build.
- **Coverage gate (the deliverable)**: `coverage.yml` green at `--threshold 70` on the Windows runner, with the measured rate reported ≥70 + headroom.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: verify the target-unit list is drawn from a real `coverage.xml` artifact (not guessed) before authoring; confirm the flip is measurement-gated.
- **Manual residue**: none — the target-selection reads a CI artifact; no manual coverage step.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — no symbols are deferred by this plan.

- **Plane/GitHub impl-TU coverage** — the fixture extension pattern applies, but those impl TUs are linked incrementally; fold them in as they enter the denominator, not in this first ramp.

## Implementation log
*(populated post-ship — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed / deferred vs the original plan, one-line rationale each)*

## Verification (actual)
*(populated post-ship — what was tested + result)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*In the SAME PR that fills the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*

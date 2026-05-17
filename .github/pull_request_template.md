<!-- Delete sections that don't apply. Keep what's useful for the reviewer. -->

## Summary

<!-- 1–3 sentences: what changed, why, and what stays the same. Link the originating plan / backlog entry / issue when one exists. -->

## Test plan

<!-- Reviewer-runnable checks. Bias toward automation; flag any manual residue. -->

- [ ] `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` (dual-target)
- [ ] `cmake --build --preset ninja-test-msys2 && ctest --output-on-failure` from `build/ninja-test-msys2/`
- [ ] `bash scripts/dev/test-all.sh` (sidecar suite)
- [ ] Manual residue: <!-- "none" if every check is automated -->

## Coverage gate override (optional — read if your PR touches `Source_Core/src/*.cpp` without adding tests)

The `coverage-gate.yml` workflow blocks PRs that change `Source_Core/src/*.cpp` without a matching test delta. Apply the **`tests-out-of-band`** label to dismiss the gate when one of these applies:

- Docs-only edits inside `Source_Core/include/*.h` (inline comments, doc-strings, license headers)
- Include-shape fixes for dual-target compatibility (no runtime behaviour change)
- Sanitizer / warning / static-analysis flag flips
- Build-system PRs that touch `Source_Core/` only transitively (e.g. CMake source-property changes)
- Mechanical refactors that produce a behaviour-preserving rename / move (already covered by existing tests)

If your PR is a behavioural change, **add tests instead of applying the label** — the gate exists so that behavioural drift gets the assertion coverage it needs.

Apply the label in the PR sidebar (Labels → `tests-out-of-band`) or via `gh pr edit <N> --add-label tests-out-of-band`.

## Plan-lock release (if this PR holds a `refs/locks/<slug>`)

If this PR claimed a plan-lock via `bash scripts/dev/lock-claim.sh <slug> ...`, add the line below somewhere in the PR body (uncomment + edit). On merge to develop, [`.github/workflows/lock-cleanup.yml`](workflows/lock-cleanup.yml) parses the line and deletes the corresponding `refs/locks/<slug>` ref. Without the line the ref stays in place and the Phase 4 staleness sweep flags it after 14 days.

<!-- lock-slug: your-slug-here -->

## Plan revision (if this PR ships a slice from a multi-PR plan)

Per [AGENTS.md § Plan revision after implementation](../AGENTS.md), the originating `docs/design/<slug>.md` should be updated in the same or next commit with:

- `## Implementation log` — `<sha> · <one-line summary>` row appended
- `## Deviations from plan` — what changed vs the plan + one-line rationale
- `## Verification` — what was tested + result

<!-- Mark "n/a" if this PR isn't part of a tracked design plan. -->

🤖 Generated with [Claude Code](https://claude.com/claude-code)
